// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// $Log:$
//
// DESCRIPTION:
//      DOOM selection menu, options, episode etc.
//      Sliders and icons. Kinda widget stuff.
//
//-----------------------------------------------------------------------------

static const char __attribute__((unused))
rcsid[] = "$Id: m_menu.c,v 1.7 1997/02/03 22:45:10 b1 Exp $";


#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <ctype.h>


#include "doomdef.h"
#include "dstrings.h"

#include "d_main.h"

#include "i_system.h"
#include "i_video.h"
#include "z_zone.h"
#include "v_video.h"
#include "w_wad.h"

#include "r_local.h"


#include "hu_stuff.h"

#include "g_game.h"

#include "m_argv.h"
#include "m_swap.h"

#include "s_sound.h"

#include "doomstat.h"

// Data.
#include "sounds.h"

#include "m_menu.h"
#include "i_net.h"
#include "d_net.h"



extern patch_t*         hu_font[HU_FONTSIZE];
extern boolean          message_dontfuckwithme;

extern boolean          chat_on;                // in heads-up code

//
// defaulted values
//
int                     mouseSensitivity;       // has default

// Show messages has default, 0 = off, 1 = on
int                     showMessages;


// Blocky mode, has default, 0 = high, 1 = normal
int                     detailLevel;
int                     screenblocks;           // has default

// temp for screenblocks (0-9)
int                     screenSize;

// -1 = no quicksave slot picked!
int                     quickSaveSlot;

 // 1 = message to be printed
int                     messageToPrint;
// ...and here is the message string!
char*                   messageString;

// message x & y
int                     messx;
int                     messy;
int                     messageLastMenuActive;

// timed message = no input from user
boolean                 messageNeedsInput;

void    (*messageRoutine)(int response);

#define SAVESTRINGSIZE  24

char gammamsg[5][26] =
{
    GAMMALVL0,
    GAMMALVL1,
    GAMMALVL2,
    GAMMALVL3,
    GAMMALVL4
};

// we are going to be entering a savegame string
int                     saveStringEnter;
int                     saveSlot;       // which slot to save in
int                     saveCharIndex;  // which char we're editing
// old save description before edit
char                    saveOldString[SAVESTRINGSIZE];

boolean                 inhelpscreens;
boolean                 menuactive;

#define SKULLXOFF               -32
#define LINEHEIGHT              16
#define MENU_YOFF               ((SCREENHEIGHT - 200) / 2)

extern boolean          sendpause;
char                    savegamestrings[10][SAVESTRINGSIZE];

char    endstring[160];


//
// MENU TYPEDEFS
//
typedef struct
{
    // 0 = no cursor here, 1 = ok, 2 = arrows ok
    short       status;

    char        name[10];

    // choice = menu item #.
    // if status = 2,
    //   choice=0:leftarrow,1:rightarrow
    void        (*routine)(int choice);

    // hotkey in menu
    char        alphaKey;
} menuitem_t;



typedef struct menu_s
{
    short               numitems;       // # of menu items
    struct menu_s*      prevMenu;       // previous menu
    menuitem_t*         menuitems;      // menu items
    void                (*routine)();   // draw routine
    short               x;
    short               y;              // x,y of menu
    short               lastOn;         // last item user was on in menu
} menu_t;

short           itemOn;                 // menu item skull is on
short           skullAnimCounter;       // skull animation counter
short           whichSkull;             // which skull to draw

// graphic name of skulls
// warning: initializer-string for array of chars is too long
char    skullName[2][/*8*/9] = {"M_SKULL1","M_SKULL2"};

// current menudef
menu_t* currentMenu;

//
// PROTOTYPES
//
void M_NewGame(int choice);
void M_Episode(int choice);
void M_ChooseSkill(int choice);
void M_LoadGame(int choice);
void M_SaveGame(int choice);
void M_Options(int choice);
void M_EndGame(int choice);
void M_ReadThis(int choice);
void M_ReadThis2(int choice);
void M_QuitDOOM(int choice);
void M_LinkCable(int choice);
void M_LinkSelect(int choice);
void M_DrawLinkMenu(void);

void M_ChangeMessages(int choice);
void M_ChangeSensitivity(int choice);
void M_SfxVol(int choice);
void M_MusicVol(int choice);
void M_ChangeDetail(int choice);
void M_SizeDisplay(int choice);
void M_StartGame(int choice);
void M_Sound(int choice);

void M_FinishReadThis(int choice);
void M_LoadSelect(int choice);
void M_SaveSelect(int choice);
void M_ReadSaveStrings(void);
void M_QuickSave(void);
void M_QuickLoad(void);

void M_DrawMainMenu(void);
void M_DrawReadThis1(void);
void M_DrawReadThis2(void);
void M_DrawNewGame(void);
void M_DrawEpisode(void);
void M_DrawOptions(void);
void M_DrawSound(void);
void M_DrawLoad(void);
void M_DrawSave(void);

void M_DrawSaveLoadBorder(int x,int y);
void M_SetupNextMenu(menu_t *menudef);
void M_DrawThermo(int x,int y,int thermWidth,int thermDot);
void M_DrawEmptyCell(menu_t *menu,int item);
void M_DrawSelCell(menu_t *menu,int item);
void M_WriteText(int x, int y, char *string);
int  M_StringWidth(char *string);
int  M_StringHeight(char *string);
void M_StartControlPanel(void);
void M_StartMessage(char *string,void *routine,boolean input);
void M_StopMessage(void);
void M_ClearMenus (void);


// Cached scale factor (fixed-point 8.8) derived from menu patch height.
static int menu_scale_fp;   // 0 = not yet computed

static void M_ComputeMenuScale(void)
{
    patch_t *ref = W_CacheLumpName("M_NGAME", PU_CACHE);
    int ref_h = SHORT(ref->height);
    int font_h = SHORT(hu_font[0]->height);
    if (font_h < 1) font_h = 1;
    menu_scale_fp = (ref_h << 8) / font_h;
    if (menu_scale_fp < 256) menu_scale_fp = 256; // at least 1x
    // Cap so scaled text never exceeds LINEHEIGHT (prevents overlap)
    int max_scale = (LINEHEIGHT << 8) / font_h;
    if (menu_scale_fp > max_scale) menu_scale_fp = max_scale;
}

// Draw a patch scaled by menu_scale_fp (fixed-point 8.8).
// Horizontal: each source column is ceil(scale) wide, vertical: Bresenham row mapping.
static void
V_DrawPatchScaled
( int           x,
  int           y,
  patch_t*      patch )
{
    int         col;
    column_t*   column;
    byte*       desttop;
    byte*       dest;
    byte*       source;
    int         w;
    int         scale = menu_scale_fp;
    int         xstep = (scale + 128) >> 8; // rounded integer x scale

    y -= (SHORT(patch->topoffset) * scale) >> 8;
    x -= (SHORT(patch->leftoffset) * scale) >> 8;

    w = SHORT(patch->width);

    for ( col = 0 ; col<w ; col++, x+=xstep)
    {
        column = (column_t *)((byte *)patch + LONG(patch->columnofs[col]));
        desttop = screens[0] + y*SCREENWIDTH + x;

        while (column->topdelta != 0xff )
        {
            source = (byte *)column + 3;
            int src_h = column->length;
            int dst_y0 = (column->topdelta * scale) >> 8;
            int dst_y1 = ((column->topdelta + src_h) * scale) >> 8;
            int dst_h = dst_y1 - dst_y0;

            dest = desttop + dst_y0 * SCREENWIDTH;

            for (int dy = 0; dy < dst_h; dy++)
            {
                int sy = (dy * src_h) / dst_h;
                byte px = source[sy];
                for (int dx = 0; dx < xstep; dx++)
                    dest[dx] = px;
                dest += SCREENWIDTH;
            }
            column = (column_t *)(  (byte *)column + column->length + 4 );
        }
    }
}

// Draw text scaled to match menu patch sizes.
static void
M_WriteTextScaled
( int           x,
  int           y,
  char*         string)
{
    char*       ch;
    int         c;
    int         cx;
    int         cy;
    int         w;
    int         scale = menu_scale_fp;
    int         xstep = (scale + 128) >> 8;

    if (!scale) { M_ComputeMenuScale(); scale = menu_scale_fp; xstep = (scale + 128) >> 8; }

    ch = string;
    cx = x;
    cy = y;

    while(1)
    {
        c = *ch++;
        if (!c)
            break;
        if (c == '\n')
        {
            cx = x;
            cy += (SHORT(hu_font[0]->height) * scale) >> 8;
            continue;
        }

        c = toupper(c) - HU_FONTSTART;
        if (c < 0 || c>= HU_FONTSIZE)
        {
            cx += 4 * xstep;
            continue;
        }

        w = SHORT (hu_font[c]->width) * xstep;
        if (cx+w > SCREENWIDTH)
            break;
        V_DrawPatchScaled(cx, cy, hu_font[c]);
        cx+=w;
    }
}


//
// DOOM MENU
//
enum
{
    newgame = 0,
    multiplayer,
    options,
    loadgame,
    savegame,
    readthis,
    main_end
} main_e;

menuitem_t MainMenu[]=
{
    {1,"M_NGAME",M_NewGame,'n'},
    {1,"",M_LinkCable,'m'},
    {1,"M_OPTION",M_Options,'o'},
    {1,"M_LOADG",M_LoadGame,'l'},
    {1,"M_SAVEG",M_SaveGame,'s'},
    // Another hickup with Special edition.
    {1,"M_RDTHIS",M_ReadThis,'r'}
};

menu_t  MainDef =
{
    main_end,
    NULL,
    MainMenu,
    M_DrawMainMenu,
    97,64,
    0
};


//
// LINK CABLE MENU
//
enum
{
    link_host_dm = 0,
    link_host_coop,
    link_join,
    link_end
} link_e;

menuitem_t LinkMenu[]=
{
    {1,"",M_LinkSelect,'d'},
    {1,"",M_LinkSelect,'c'},
    {1,"",M_LinkSelect,'j'}
};

menu_t  LinkDef =
{
    link_end,
    &MainDef,
    LinkMenu,
    M_DrawLinkMenu,
    54,100,
    0
};


//
// EPISODE SELECT
//
enum
{
    ep1,
    ep2,
    ep3,
    ep4,
    ep_end
} episodes_e;

menuitem_t EpisodeMenu[]=
{
    {1,"M_EPI1", M_Episode,'k'},
    {1,"M_EPI2", M_Episode,'t'},
    {1,"M_EPI3", M_Episode,'i'},
    {1,"M_EPI4", M_Episode,'t'}
};

menu_t  EpiDef =
{
    ep_end,             // # of menu items
    &MainDef,           // previous menu
    EpisodeMenu,        // menuitem_t ->
    M_DrawEpisode,      // drawing routine ->
    48,63,              // x,y
    ep1                 // lastOn
};

//
// NEW GAME
//
enum
{
    killthings,
    toorough,
    hurtme,
    violence,
    nightmare,
    newg_end
} newgame_e;

menuitem_t NewGameMenu[]=
{
    {1,"M_JKILL",       M_ChooseSkill, 'i'},
    {1,"M_ROUGH",       M_ChooseSkill, 'h'},
    {1,"M_HURT",        M_ChooseSkill, 'h'},
    {1,"M_ULTRA",       M_ChooseSkill, 'u'},
    {1,"M_NMARE",       M_ChooseSkill, 'n'}
};

menu_t  NewDef =
{
    newg_end,           // # of menu items
    &EpiDef,            // previous menu
    NewGameMenu,        // menuitem_t ->
    M_DrawNewGame,      // drawing routine ->
    48,63,              // x,y
    hurtme              // lastOn
};



//
// OPTIONS MENU
//
enum
{
    endgame,
    messages,
    detail,
    scrnsize,
    option_empty1,
    mousesens,
    option_empty2,
    soundvol,
    opt_end
} options_e;

menuitem_t OptionsMenu[]=
{
    {1,"M_ENDGAM",      M_EndGame,'e'},
    {1,"M_MESSG",       M_ChangeMessages,'m'},
    {1,"M_DETAIL",      M_ChangeDetail,'g'},
    {2,"M_SCRNSZ",      M_SizeDisplay,'s'},
    {-1,"",0},
    {2,"M_MSENS",       M_ChangeSensitivity,'m'},
    {-1,"",0},
    {1,"M_SVOL",        M_Sound,'s'}
};

menu_t  OptionsDef =
{
    opt_end,
    &MainDef,
    OptionsMenu,
    M_DrawOptions,
    60,37,
    0
};

//
// Read This! MENU 1 & 2
//
enum
{
    rdthsempty1,
    read1_end
} read_e;

menuitem_t ReadMenu1[] =
{
    {1,"",M_ReadThis2,0}
};

menu_t  ReadDef1 =
{
    read1_end,
    &MainDef,
    ReadMenu1,
    M_DrawReadThis1,
    280,185,
    0
};

enum
{
    rdthsempty2,
    read2_end
} read_e2;

menuitem_t ReadMenu2[]=
{
    {1,"",M_FinishReadThis,0}
};

menu_t  ReadDef2 =
{
    read2_end,
    &ReadDef1,
    ReadMenu2,
    M_DrawReadThis2,
    330,175,
    0
};

//
// SOUND VOLUME MENU
//
enum
{
    sfx_vol,
    sfx_empty1,
    music_vol,
    sfx_empty2,
    sound_end
} sound_e;

menuitem_t SoundMenu[]=
{
    {2,"M_SFXVOL",M_SfxVol,'s'},
    {-1,"",0},
    {2,"M_MUSVOL",M_MusicVol,'m'},
    {-1,"",0}
};

menu_t  SoundDef =
{
    sound_end,
    &OptionsDef,
    SoundMenu,
    M_DrawSound,
    80,64,
    0
};

//
// LOAD GAME MENU
//
enum
{
    load1,
    load2,
    load3,
    load4,
    load5,
    load6,
    load_end
} load_e;

menuitem_t LoadMenu[]=
{
    {1,"", M_LoadSelect,'1'},
    {1,"", M_LoadSelect,'2'},
    {1,"", M_LoadSelect,'3'},
    {1,"", M_LoadSelect,'4'},
    {1,"", M_LoadSelect,'5'},
    {1,"", M_LoadSelect,'6'}
};

menu_t  LoadDef =
{
    load_end,
    &MainDef,
    LoadMenu,
    M_DrawLoad,
    80,54,
    0
};

//
// SAVE GAME MENU
//
menuitem_t SaveMenu[]=
{
    {1,"", M_SaveSelect,'1'},
    {1,"", M_SaveSelect,'2'},
    {1,"", M_SaveSelect,'3'},
    {1,"", M_SaveSelect,'4'},
    {1,"", M_SaveSelect,'5'},
    {1,"", M_SaveSelect,'6'}
};

menu_t  SaveDef =
{
    load_end,
    &MainDef,
    SaveMenu,
    M_DrawSave,
    80,54,
    0
};


//
// M_ReadSaveStrings
//  read the strings from the savegame files
//
void M_ReadSaveStrings(void)
{
    int             handle;
    int             i;
    char    name[256];

    for (i = 0;i < load_end;i++)
    {
        sprintf(name,SAVEGAMENAME"%d.dsg",i);

        handle = open (name, O_RDONLY | 0, 0666);
        if (handle == -1)
        {
            strcpy(&savegamestrings[i][0],EMPTYSTRING);
            LoadMenu[i].status = 0;
            continue;
        }
        read (handle, &savegamestrings[i], SAVESTRINGSIZE);
        close (handle);
        LoadMenu[i].status = 1;
    }
}


//
// M_LoadGame & Cie.
//
void M_DrawLoad(void)
{
    int             i;

    V_DrawPatchDirect (72,28+MENU_YOFF,0,W_CacheLumpName("M_LOADG",PU_CACHE));  /* title stays hardcoded */
    for (i = 0;i < load_end; i++)
    {
        M_DrawSaveLoadBorder(LoadDef.x,LoadDef.y+LINEHEIGHT*i);
        M_WriteText(LoadDef.x,LoadDef.y+LINEHEIGHT*i,savegamestrings[i]);
    }
}



//
// Draw border for the savegame description
//
void M_DrawSaveLoadBorder(int x,int y)
{
    int             i;

    V_DrawPatchDirect (x-8,y+7,0,W_CacheLumpName("M_LSLEFT",PU_CACHE));

    for (i = 0;i < 24;i++)
    {
        V_DrawPatchDirect (x,y+7,0,W_CacheLumpName("M_LSCNTR",PU_CACHE));
        x += 8;
    }

    V_DrawPatchDirect (x,y+7,0,W_CacheLumpName("M_LSRGHT",PU_CACHE));
}



//
// User wants to load this game
//
void M_LoadSelect(int choice)
{
    char    name[256];

    if (M_CheckParm("-cdrom"))
        sprintf(name,"c:\\doomdata\\"SAVEGAMENAME"%d.dsg",choice);
    else
        sprintf(name,SAVEGAMENAME"%d.dsg",choice);
    G_LoadGame (name);
    M_ClearMenus ();
}

//
// Selected from DOOM menu
//
void M_LoadGame (int choice)
{
    if (netgame)
    {
        M_StartMessage(LOADNET,NULL,false);
        return;
    }

    M_SetupNextMenu(&LoadDef);
    M_ReadSaveStrings();
}


//
//  M_SaveGame & Cie.
//
void M_DrawSave(void)
{
    int             i;

    V_DrawPatchDirect (72,28+MENU_YOFF,0,W_CacheLumpName("M_SAVEG",PU_CACHE));
    for (i = 0;i < load_end; i++)
    {
        M_DrawSaveLoadBorder(LoadDef.x,LoadDef.y+LINEHEIGHT*i);
        M_WriteText(LoadDef.x,LoadDef.y+LINEHEIGHT*i,savegamestrings[i]);
    }

    if (saveStringEnter)
    {
        i = M_StringWidth(savegamestrings[saveSlot]);
        M_WriteText(LoadDef.x + i,LoadDef.y+LINEHEIGHT*saveSlot,"_");
    }
}

//
// M_Responder calls this when user is finished
//
void M_DoSave(int slot)
{
    G_SaveGame (slot,savegamestrings[slot]);
    M_ClearMenus ();

    // PICK QUICKSAVE SLOT YET?
    if (quickSaveSlot == -2)
        quickSaveSlot = slot;
}

//
// User wants to save. Start string input for M_Responder
//
void M_SaveSelect(int choice)
{
#ifdef POCKET_DOOM
    /* No keyboard — auto-generate a name from the current level. */
    saveSlot = choice;
    if (gamemode == commercial)
        sprintf(savegamestrings[choice], "MAP%02d", gamemap);
    else
        sprintf(savegamestrings[choice], "E%dM%d", gameepisode, gamemap);
    M_DoSave(choice);
#else
    // we are going to be intercepting all chars
    saveStringEnter = 1;

    saveSlot = choice;
    strcpy(saveOldString,savegamestrings[choice]);
    if (!strcmp(savegamestrings[choice],EMPTYSTRING))
        savegamestrings[choice][0] = 0;
    saveCharIndex = strlen(savegamestrings[choice]);
#endif
}

//
// Selected from DOOM menu
//
void M_SaveGame (int choice)
{
    if (!usergame)
    {
        M_StartMessage(SAVEDEAD,NULL,false);
        return;
    }

    if (gamestate != GS_LEVEL)
        return;

    M_SetupNextMenu(&SaveDef);
    M_ReadSaveStrings();
}



//
//      M_QuickSave
//
char    tempstring[96];

void M_QuickSaveResponse(int ch)
{
    if (ch == 'y')
    {
        M_DoSave(quickSaveSlot);
        S_StartSound(NULL,sfx_swtchx);
    }
}

void M_QuickSave(void)
{
    if (!usergame)
    {
        S_StartSound(NULL,sfx_oof);
        return;
    }

    if (gamestate != GS_LEVEL)
        return;

    if (quickSaveSlot < 0)
    {
        M_StartControlPanel();
        M_ReadSaveStrings();
        M_SetupNextMenu(&SaveDef);
        quickSaveSlot = -2;     // means to pick a slot now
        return;
    }
    sprintf(tempstring,QSPROMPT,savegamestrings[quickSaveSlot]);
    M_StartMessage(tempstring,M_QuickSaveResponse,true);
}



//
// M_QuickLoad
//
void M_QuickLoadResponse(int ch)
{
    if (ch == 'y')
    {
        M_LoadSelect(quickSaveSlot);
        S_StartSound(NULL,sfx_swtchx);
    }
}


void M_QuickLoad(void)
{
    if (netgame)
    {
        M_StartMessage(QLOADNET,NULL,false);
        return;
    }

    if (quickSaveSlot < 0)
    {
        M_StartMessage(QSAVESPOT,NULL,false);
        return;
    }
    sprintf(tempstring,QLPROMPT,savegamestrings[quickSaveSlot]);
    M_StartMessage(tempstring,M_QuickLoadResponse,true);
}




//
// Read This Menus
// Had a "quick hack to fix romero bug"
//
void M_DrawReadThis1(void)
{
    inhelpscreens = true;
    switch ( gamemode )
    {
      case commercial:
        V_DrawPatchDirect (0,MENU_YOFF,0,W_CacheLumpName("HELP",PU_CACHE));
        break;
      case shareware:
      case registered:
      case retail:
        V_DrawPatchDirect (0,MENU_YOFF,0,W_CacheLumpName("HELP1",PU_CACHE));
        break;
      default:
        break;
    }
    return;
}



//
// Read This Menus - optional second page.
//
void M_DrawReadThis2(void)
{
    inhelpscreens = true;
    switch ( gamemode )
    {
      case retail:
      case commercial:
        // This hack keeps us from having to change menus.
        V_DrawPatchDirect (0,MENU_YOFF,0,W_CacheLumpName("CREDIT",PU_CACHE));
        break;
      case shareware:
      case registered:
        V_DrawPatchDirect (0,MENU_YOFF,0,W_CacheLumpName("HELP2",PU_CACHE));
        break;
      default:
        break;
    }
    return;
}


//
// Change Sfx & Music volumes
//
void M_DrawSound(void)
{
    V_DrawPatchDirect (60,38+MENU_YOFF,0,W_CacheLumpName("M_SVOL",PU_CACHE));

    M_DrawThermo(SoundDef.x,SoundDef.y+LINEHEIGHT*(sfx_vol+1),
                 16,snd_SfxVolume);

    M_DrawThermo(SoundDef.x,SoundDef.y+LINEHEIGHT*(music_vol+1),
                 16,snd_MusicVolume);
}

void M_Sound(int choice)
{
    M_SetupNextMenu(&SoundDef);
}

void M_SfxVol(int choice)
{
    switch(choice)
    {
      case 0:
        if (snd_SfxVolume)
            snd_SfxVolume--;
        break;
      case 1:
        if (snd_SfxVolume < 15)
            snd_SfxVolume++;
        break;
    }

    S_SetSfxVolume(snd_SfxVolume /* *8 */);
}

void M_MusicVol(int choice)
{
    switch(choice)
    {
      case 0:
        if (snd_MusicVolume)
            snd_MusicVolume--;
        break;
      case 1:
        if (snd_MusicVolume < 15)
            snd_MusicVolume++;
        break;
    }

    S_SetMusicVolume(snd_MusicVolume /* *8 */);
}




//
// M_DrawMainMenu
//
void M_DrawMainMenu(void)
{
    V_DrawPatchDirect (94,2+MENU_YOFF,0,W_CacheLumpName("M_DOOM",PU_CACHE));

    // Draw "Multiplayer" scaled to match menu patch height.
    // Use the same y as M_Drawer would for this item slot.
    if (!menu_scale_fp) M_ComputeMenuScale();
    M_WriteTextScaled(97, MainDef.y + multiplayer * LINEHEIGHT, "Multiplayer");
}




//
// M_NewGame
//
void M_DrawNewGame(void)
{
    V_DrawPatchDirect (96,14+MENU_YOFF,0,W_CacheLumpName("M_NEWG",PU_CACHE));
    V_DrawPatchDirect (54,38+MENU_YOFF,0,W_CacheLumpName("M_SKILL",PU_CACHE));
}

void M_NewGame(int choice)
{
    if (netgame && !demoplayback)
    {
        M_StartMessage(NEWGAME,NULL,false);
        return;
    }

    if ( gamemode == commercial )
        M_SetupNextMenu(&NewDef);
    else
        M_SetupNextMenu(&EpiDef);
}


//
//      M_Episode
//
int     epi;

void M_DrawEpisode(void)
{
    V_DrawPatchDirect (54,38+MENU_YOFF,0,W_CacheLumpName("M_EPISOD",PU_CACHE));
}

void M_VerifyNightmare(int ch)
{
    if (ch != 'y')
        return;

    G_DeferedInitNew(nightmare,epi+1,1);
    M_ClearMenus ();
}

void M_ChooseSkill(int choice)
{
    if (choice == nightmare)
    {
        M_StartMessage(NIGHTMARE,M_VerifyNightmare,true);
        return;
    }

    G_DeferedInitNew(choice,epi+1,1);
    M_ClearMenus ();
}

void M_Episode(int choice)
{
    if ( (gamemode == shareware)
         && choice)
    {
        M_StartMessage(SWSTRING,NULL,false);
        M_SetupNextMenu(&ReadDef1);
        return;
    }

    // Yet another hack...
    if ( (gamemode == registered)
         && (choice > 2))
    {
      fprintf( stderr,
               "M_Episode: 4th episode requires UltimateDOOM\n");
      choice = 0;
    }

    epi = choice;
    M_SetupNextMenu(&NewDef);
}



//
// M_LinkCable — Link cable multiplayer menu
//

// Externs from d_net.c needed for network setup
extern doomdata_t*  netbuffer;
extern boolean      nodeingame[];
extern int          nettics[];
extern boolean      remoteresend[];
extern int          resendto[];
extern int          resendcount[];
extern int          nodeforplayer[];
extern int          ticdup;
extern int          maxsend;
extern int          maketic;
extern int          lastnettic;
extern int          skiptics;
extern int          gametic;
extern boolean      advancedemo;
extern void         D_ResetNetSync(void);
extern void         D_ArbitrateNetStart(void);

void M_DrawLinkMenu(void)
{
    M_WriteTextScaled(78, 74, "Multiplayer");

    M_WriteTextScaled(LinkDef.x, LinkDef.y, "Host Deathmatch");
    M_WriteTextScaled(LinkDef.x, LinkDef.y + LINEHEIGHT, "Host Co-op");
    M_WriteTextScaled(LinkDef.x, LinkDef.y + 2 * LINEHEIGHT, "Join Game");
}

void M_LinkCable(int choice)
{
    if (!I_LinkHasHardware()) {
        M_StartMessage("Link cable hardware\nnot detected.\n\npress a key.",
                        NULL, false);
        return;
    }
    if (netgame && !demoplayback) {
        M_StartMessage("Already in a\nnetwork game.\n\npress a key.",
                        NULL, false);
        return;
    }
    M_SetupNextMenu(&LinkDef);
}

void M_LinkSelect(int choice)
{
    int is_host, dm;
    int i;

    switch (choice) {
    case link_host_dm:   is_host = 1; dm = 1; break;
    case link_host_coop: is_host = 1; dm = 0; break;
    case link_join:      is_host = 0; dm = 0; break;
    default: return;
    }

    // Draw connection status on screen
    memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);
    M_WriteText(70, 80, is_host ? "Waiting for player..." : "Joining host...");
    M_WriteText(108, 110, "B: cancel");
    I_FinishUpdate();

    // Blocking connection attempt
    int result = I_LinkConnect(is_host);

    if (result < 0) {
        // Failed or cancelled — show briefly, return to menu
        memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);
        M_WriteText(80, 90, "Connection failed.");
        I_FinishUpdate();
        {
            int t0 = I_GetTime();
            while (I_GetTime() - t0 < 70) ;
        }
        return;
    }

    // Connected — configure network state
    I_LinkSetupNet(is_host, dm);
    consoleplayer = displayplayer = doomcom->consoleplayer;
    netbuffer = &doomcom->data;

    // Show connected status
    memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);
    M_WriteText(60, 90, is_host
        ? "Connected as Player 1" : "Connected as Player 2");
    I_FinishUpdate();
    {
        int t0 = I_GetTime();
        while (I_GetTime() - t0 < 35) ;
    }

    // Exit demo state — HGetPacket returns false while demoplayback is true
    demoplayback = false;
    advancedemo = false;
    usergame = true;

    // Minimal pre-arbitrate reset so D_ArbitrateNetStart can work
    maketic = 0;
    gametic = 0;
    netbuffer = &doomcom->data;

    // Close menu NOW — before any blocking calls, so I_StartTic
    // (called from CheckAbort) uses gameplay button mapping, not menu mapping
    M_ClearMenus();
    {
        // We're called from inside D_ProcessEvents' for loop. After we return,
        // the loop does eventtail = (eventtail+1)&63 before checking the exit
        // condition. So set eventtail one BEFORE eventhead, so the increment
        // lands exactly on eventhead and the loop exits cleanly.
        extern int eventtail, eventhead;
        extern int messageToPrint;
        eventtail = (eventhead - 1) & (MAXEVENTS - 1);
        messageToPrint = 0;
    }

    // Show sync status
    memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);
    M_WriteText(60, 90, "Syncing game settings...");
    I_FinishUpdate();

    // Host sends game settings; join receives them
    if (is_host) {
        startepisode = gameepisode > 0 ? gameepisode : 1;
        startmap = 1;
        startskill = gameskill;
    }

    D_ArbitrateNetStart();

    // Full reset of all timing and tic state after arbitration
    // so both sides start the game loop from a clean slate
    D_ResetNetSync();

    // Finalize network state
    ticdup = doomcom->ticdup;
    maxsend = BACKUPTICS / (2 * ticdup) - 1;
    if (maxsend < 1) maxsend = 1;

    for (i = 0; i < doomcom->numplayers; i++)
        playeringame[i] = true;
    for (i = 0; i < doomcom->numnodes; i++)
        nodeingame[i] = true;
    nodeforplayer[0] = 0;
    nodeforplayer[1] = 1;

    // Start the game — call G_InitNew directly instead of G_DeferedInitNew
    // because G_DoNewGame resets netgame/deathmatch/playeringame/consoleplayer
    G_InitNew(startskill, startepisode, startmap);
    gameaction = ga_nothing;

    // Restore net state that G_InitNew may have touched
    netgame = true;
    deathmatch = dm;
    consoleplayer = displayplayer = doomcom->consoleplayer;

    // Final cleanup: flush any events accumulated during init,
    // ensure menu is closed.  Use (eventhead-1) so D_ProcessEvents'
    // for-loop increment lands exactly on eventhead.
    {
        extern int eventtail, eventhead;
        extern int messageToPrint;
        eventtail = (eventhead - 1) & (MAXEVENTS - 1);
        messageToPrint = 0;
    }
    menuactive = 0;
}



//
// M_Options
//
char    detailNames[2][9]       = {"M_GDHIGH","M_GDLOW"};
char    msgNames[2][9]          = {"M_MSGOFF","M_MSGON"};


void M_DrawOptions(void)
{
    V_DrawPatchDirect (108,15+MENU_YOFF,0,W_CacheLumpName("M_OPTTTL",PU_CACHE));

    V_DrawPatchDirect (OptionsDef.x + 175,OptionsDef.y+LINEHEIGHT*detail,0,
                       W_CacheLumpName(detailNames[detailLevel],PU_CACHE));

    V_DrawPatchDirect (OptionsDef.x + 120,OptionsDef.y+LINEHEIGHT*messages,0,
                       W_CacheLumpName(msgNames[showMessages],PU_CACHE));

    M_DrawThermo(OptionsDef.x,OptionsDef.y+LINEHEIGHT*(mousesens+1),
                 10,mouseSensitivity);

    M_DrawThermo(OptionsDef.x,OptionsDef.y+LINEHEIGHT*(scrnsize+1),
                 9,screenSize);
}

void M_Options(int choice)
{
    M_SetupNextMenu(&OptionsDef);
}



//
//      Toggle messages on/off
//
void M_ChangeMessages(int choice)
{
    // warning: unused parameter `int choice'
    choice = 0;
    showMessages = 1 - showMessages;

    if (!showMessages)
        players[consoleplayer].message = MSGOFF;
    else
        players[consoleplayer].message = MSGON ;

    message_dontfuckwithme = true;
}


//
// M_EndGame
//
void M_EndGameResponse(int ch)
{
    if (ch != 'y')
        return;

    currentMenu->lastOn = itemOn;
    M_ClearMenus ();
    D_StartTitle ();
}

void M_EndGame(int choice)
{
    choice = 0;
    if (!usergame)
    {
        S_StartSound(NULL,sfx_oof);
        return;
    }

    if (netgame)
    {
        M_StartMessage(NETEND,NULL,false);
        return;
    }

    M_StartMessage(ENDGAME,M_EndGameResponse,true);
}




//
// M_ReadThis
//
void M_ReadThis(int choice)
{
    choice = 0;
    M_SetupNextMenu(&ReadDef1);
}

void M_ReadThis2(int choice)
{
    choice = 0;
    M_SetupNextMenu(&ReadDef2);
}

void M_FinishReadThis(int choice)
{
    choice = 0;
    M_SetupNextMenu(&MainDef);
}




//
// M_QuitDOOM
//
int     quitsounds[8] =
{
    sfx_pldeth,
    sfx_dmpain,
    sfx_popain,
    sfx_slop,
    sfx_telept,
    sfx_posit1,
    sfx_posit3,
    sfx_sgtatk
};

int     quitsounds2[8] =
{
    sfx_vilact,
    sfx_getpow,
    sfx_boscub,
    sfx_slop,
    sfx_skeswg,
    sfx_kntdth,
    sfx_bspact,
    sfx_sgtatk
};



void M_QuitResponse(int ch)
{
    if (ch != 'y')
        return;
    if (!netgame)
    {
        if (gamemode == commercial)
            S_StartSound(NULL,quitsounds2[(gametic>>2)&7]);
        else
            S_StartSound(NULL,quitsounds[(gametic>>2)&7]);
        I_WaitVBL(105);
    }
    I_Quit ();
}




void M_QuitDOOM(int choice)
{
  // We pick index 0 which is language sensitive,
  //  or one at random, between 1 and maximum number.
  if (language != english )
    sprintf(endstring,"%s\n\n"DOSY, endmsg[0] );
  else
    sprintf(endstring,"%s\n\n"DOSY, endmsg[ (gametic%(NUM_QUITMESSAGES-2))+1 ]);

  M_StartMessage(endstring,M_QuitResponse,true);
}




void M_ChangeSensitivity(int choice)
{
    switch(choice)
    {
      case 0:
        if (mouseSensitivity)
            mouseSensitivity--;
        break;
      case 1:
        if (mouseSensitivity < 9)
            mouseSensitivity++;
        break;
    }
}




void M_ChangeDetail(int choice)
{
    choice = 0;
    detailLevel = 1 - detailLevel;

    // FIXME - does not work. Remove anyway?
    fprintf( stderr, "M_ChangeDetail: low detail mode n.a.\n");

    return;

    /*R_SetViewSize (screenblocks, detailLevel);

    if (!detailLevel)
        players[consoleplayer].message = DETAILHI;
    else
        players[consoleplayer].message = DETAILLO;*/
}




void M_SizeDisplay(int choice)
{
    switch(choice)
    {
      case 0:
        if (screenSize > 0)
        {
            screenblocks--;
            screenSize--;
        }
        break;
      case 1:
        if (screenSize < 8)
        {
            screenblocks++;
            screenSize++;
        }
        break;
    }


    R_SetViewSize (screenblocks, detailLevel);
}




//
//      Menu Functions
//
void
M_DrawThermo
( int   x,
  int   y,
  int   thermWidth,
  int   thermDot )
{
    int         xx;
    int         i;

    xx = x;
    V_DrawPatchDirect (xx,y,0,W_CacheLumpName("M_THERML",PU_CACHE));
    xx += 8;
    for (i=0;i<thermWidth;i++)
    {
        V_DrawPatchDirect (xx,y,0,W_CacheLumpName("M_THERMM",PU_CACHE));
        xx += 8;
    }
    V_DrawPatchDirect (xx,y,0,W_CacheLumpName("M_THERMR",PU_CACHE));

    V_DrawPatchDirect ((x+8) + thermDot*8,y,
                       0,W_CacheLumpName("M_THERMO",PU_CACHE));
}



void
M_DrawEmptyCell
( menu_t*       menu,
  int           item )
{
    V_DrawPatchDirect (menu->x - 10,        menu->y+item*LINEHEIGHT - 1, 0,
                       W_CacheLumpName("M_CELL1",PU_CACHE));
}

void
M_DrawSelCell
( menu_t*       menu,
  int           item )
{
    V_DrawPatchDirect (menu->x - 10,        menu->y+item*LINEHEIGHT - 1, 0,
                       W_CacheLumpName("M_CELL2",PU_CACHE));
}


void
M_StartMessage
( char*         string,
  void*         routine,
  boolean       input )
{
    messageLastMenuActive = menuactive;
    messageToPrint = 1;
    messageString = string;
    messageRoutine = routine;
    messageNeedsInput = input;
    menuactive = true;
    return;
}



void M_StopMessage(void)
{
    menuactive = messageLastMenuActive;
    messageToPrint = 0;
}



//
// Find string width from hu_font chars
//
int M_StringWidth(char* string)
{
    int             i;
    int             w = 0;
    int             c;

    for (i = 0;i < strlen(string);i++)
    {
        c = toupper(string[i]) - HU_FONTSTART;
        if (c < 0 || c >= HU_FONTSIZE)
            w += 4;
        else
            w += SHORT (hu_font[c]->width);
    }

    return w;
}



//
//      Find string height from hu_font chars
//
int M_StringHeight(char* string)
{
    int             i;
    int             h;
    int             height = SHORT(hu_font[0]->height);

    h = height;
    for (i = 0;i < strlen(string);i++)
        if (string[i] == '\n')
            h += height;

    return h;
}


//
//      Write a string using the hu_font
//
void
M_WriteText
( int           x,
  int           y,
  char*         string)
{
    int         w;
    char*       ch;
    int         c;
    int         cx;
    int         cy;


    ch = string;
    cx = x;
    cy = y;

    while(1)
    {
        c = *ch++;
        if (!c)
            break;
        if (c == '\n')
        {
            cx = x;
            cy += 12;
            continue;
        }

        c = toupper(c) - HU_FONTSTART;
        if (c < 0 || c>= HU_FONTSIZE)
        {
            cx += 4;
            continue;
        }

        w = SHORT (hu_font[c]->width);
        if (cx+w > SCREENWIDTH)
            break;
        V_DrawPatchDirect(cx, cy, 0, hu_font[c]);
        cx+=w;
    }
}



//
// CONTROL PANEL
//

//
// M_Responder
//
boolean M_Responder (event_t* ev)
{
    int             ch;
    int             i;
    static  int     joywait = 0;
    static  int     mousewait = 0;
    static  int     mousey = 0;
    static  int     lasty = 0;
    static  int     mousex = 0;
    static  int     lastx = 0;

    ch = -1;

    if (ev->type == ev_joystick && joywait < I_GetTime())
    {
        if (ev->data3 == -1)
        {
            ch = KEY_UPARROW;
            joywait = I_GetTime() + 5;
        }
        else if (ev->data3 == 1)
        {
            ch = KEY_DOWNARROW;
            joywait = I_GetTime() + 5;
        }

        if (ev->data2 == -1)
        {
            ch = KEY_LEFTARROW;
            joywait = I_GetTime() + 2;
        }
        else if (ev->data2 == 1)
        {
            ch = KEY_RIGHTARROW;
            joywait = I_GetTime() + 2;
        }

        if (ev->data1&1)
        {
            ch = KEY_ENTER;
            joywait = I_GetTime() + 5;
        }
        if (ev->data1&2)
        {
            ch = KEY_BACKSPACE;
            joywait = I_GetTime() + 5;
        }
    }
    else
    {
        if (ev->type == ev_mouse && mousewait < I_GetTime())
        {
            mousey += ev->data3;
            if (mousey < lasty-30)
            {
                ch = KEY_DOWNARROW;
                mousewait = I_GetTime() + 5;
                mousey = lasty -= 30;
            }
            else if (mousey > lasty+30)
            {
                ch = KEY_UPARROW;
                mousewait = I_GetTime() + 5;
                mousey = lasty += 30;
            }

            mousex += ev->data2;
            if (mousex < lastx-30)
            {
                ch = KEY_LEFTARROW;
                mousewait = I_GetTime() + 5;
                mousex = lastx -= 30;
            }
            else if (mousex > lastx+30)
            {
                ch = KEY_RIGHTARROW;
                mousewait = I_GetTime() + 5;
                mousex = lastx += 30;
            }

            if (ev->data1&1)
            {
                ch = KEY_ENTER;
                mousewait = I_GetTime() + 15;
            }

            if (ev->data1&2)
            {
                ch = KEY_BACKSPACE;
                mousewait = I_GetTime() + 15;
            }
        }
        else
            if (ev->type == ev_keydown)
            {
                ch = ev->data1;
            }
    }

    if (ch == -1)
        return false;

    // Gamepad: remap fire/use keys for menu navigation
    // A button (fire=RCTRL) → confirm, B button (use=space) → back
    if (messageToPrint)
    {
        if (ch == KEY_RCTRL) ch = 'y';           // A = yes/confirm
    }
    else if (menuactive)
    {
        if (ch == KEY_RCTRL) ch = KEY_ENTER;     // A = confirm
        if (ch == ' ')       ch = KEY_BACKSPACE;  // B = back
    }

    // Save Game string input
    if (saveStringEnter)
    {
        switch(ch)
        {
          case KEY_BACKSPACE:
            if (saveCharIndex > 0)
            {
                saveCharIndex--;
                savegamestrings[saveSlot][saveCharIndex] = 0;
            }
            break;

          case KEY_ESCAPE:
            saveStringEnter = 0;
            strcpy(&savegamestrings[saveSlot][0],saveOldString);
            break;

          case KEY_ENTER:
            saveStringEnter = 0;
            if (savegamestrings[saveSlot][0])
                M_DoSave(saveSlot);
            break;

          default:
            ch = toupper(ch);
            if (ch != 32)
                if (ch-HU_FONTSTART < 0 || ch-HU_FONTSTART >= HU_FONTSIZE)
                    break;
            if (ch >= 32 && ch <= 127 &&
                saveCharIndex < SAVESTRINGSIZE-1 &&
                M_StringWidth(savegamestrings[saveSlot]) <
                (SAVESTRINGSIZE-2)*8)
            {
                savegamestrings[saveSlot][saveCharIndex++] = ch;
                savegamestrings[saveSlot][saveCharIndex] = 0;
            }
            break;
        }
        return true;
    }

    // Take care of any messages that need input
    if (messageToPrint)
    {
        if (messageNeedsInput == true &&
            !(ch == ' ' || ch == 'n' || ch == 'y' || ch == KEY_ESCAPE))
            return false;

        menuactive = messageLastMenuActive;
        messageToPrint = 0;
        if (messageRoutine)
            messageRoutine(ch);

        menuactive = false;
        S_StartSound(NULL,sfx_swtchx);
        return true;
    }

    if (devparm && ch == KEY_F1)
    {
        G_ScreenShot ();
        return true;
    }


    // F-Keys
    if (!menuactive)
        switch(ch)
        {
          case KEY_MINUS:         // Screen size down
            if (automapactive || chat_on)
                return false;
            M_SizeDisplay(0);
            S_StartSound(NULL,sfx_stnmov);
            return true;

          case KEY_EQUALS:        // Screen size up
            if (automapactive || chat_on)
                return false;
            M_SizeDisplay(1);
            S_StartSound(NULL,sfx_stnmov);
            return true;

          case KEY_F1:            // Help key
            M_StartControlPanel ();

            if ( gamemode == retail )
              currentMenu = &ReadDef2;
            else
              currentMenu = &ReadDef1;

            itemOn = 0;
            S_StartSound(NULL,sfx_swtchn);
            return true;

          case KEY_F2:            // Save
            M_StartControlPanel();
            S_StartSound(NULL,sfx_swtchn);
            M_SaveGame(0);
            return true;

          case KEY_F3:            // Load
            M_StartControlPanel();
            S_StartSound(NULL,sfx_swtchn);
            M_LoadGame(0);
            return true;

          case KEY_F4:            // Sound Volume
            M_StartControlPanel ();
            currentMenu = &SoundDef;
            itemOn = sfx_vol;
            S_StartSound(NULL,sfx_swtchn);
            return true;

          case KEY_F5:            // Detail toggle
            M_ChangeDetail(0);
            S_StartSound(NULL,sfx_swtchn);
            return true;

          case KEY_F6:            // Quicksave
            S_StartSound(NULL,sfx_swtchn);
            M_QuickSave();
            return true;

          case KEY_F7:            // End game
            S_StartSound(NULL,sfx_swtchn);
            M_EndGame(0);
            return true;

          case KEY_F8:            // Toggle messages
            M_ChangeMessages(0);
            S_StartSound(NULL,sfx_swtchn);
            return true;

          case KEY_F9:            // Quickload
            S_StartSound(NULL,sfx_swtchn);
            M_QuickLoad();
            return true;

          case KEY_F11:           // gamma toggle
            usegamma++;
            if (usegamma > 4)
                usegamma = 0;
            players[consoleplayer].message = gammamsg[usegamma];
            I_SetPalette (W_CacheLumpName ("PLAYPAL",PU_CACHE));
            return true;

        }


    // Pop-up menu?
    if (!menuactive)
    {
        if (ch == KEY_ESCAPE)
        {
            M_StartControlPanel ();
            S_StartSound(NULL,sfx_swtchn);
            return true;
        }
        return false;
    }


    // Keys usable within menu
    switch (ch)
    {
      case KEY_DOWNARROW:
        do
        {
            if (itemOn+1 > currentMenu->numitems-1)
                itemOn = 0;
            else itemOn++;
            S_StartSound(NULL,sfx_pstop);
        } while(currentMenu->menuitems[itemOn].status==-1);
        return true;

      case KEY_UPARROW:
        do
        {
            if (!itemOn)
                itemOn = currentMenu->numitems-1;
            else itemOn--;
            S_StartSound(NULL,sfx_pstop);
        } while(currentMenu->menuitems[itemOn].status==-1);
        return true;

      case KEY_LEFTARROW:
        if (currentMenu->menuitems[itemOn].routine &&
            currentMenu->menuitems[itemOn].status == 2)
        {
            S_StartSound(NULL,sfx_stnmov);
            currentMenu->menuitems[itemOn].routine(0);
        }
        return true;

      case KEY_RIGHTARROW:
        if (currentMenu->menuitems[itemOn].routine &&
            currentMenu->menuitems[itemOn].status == 2)
        {
            S_StartSound(NULL,sfx_stnmov);
            currentMenu->menuitems[itemOn].routine(1);
        }
        return true;

      case KEY_ENTER:
        if (currentMenu->menuitems[itemOn].routine &&
            currentMenu->menuitems[itemOn].status)
        {
            currentMenu->lastOn = itemOn;
            if (currentMenu->menuitems[itemOn].status == 2)
            {
                currentMenu->menuitems[itemOn].routine(1);      // right arrow
                S_StartSound(NULL,sfx_stnmov);
            }
            else
            {
                currentMenu->menuitems[itemOn].routine(itemOn);
                S_StartSound(NULL,sfx_pistol);
            }
        }
        return true;

      case KEY_ESCAPE:
        currentMenu->lastOn = itemOn;
        M_ClearMenus ();
        S_StartSound(NULL,sfx_swtchx);
        return true;

      case KEY_BACKSPACE:
        currentMenu->lastOn = itemOn;
        if (currentMenu->prevMenu)
        {
            M_SaveDefaults();  /* Persist settings on menu back */
            currentMenu = currentMenu->prevMenu;
            itemOn = currentMenu->lastOn;
            S_StartSound(NULL,sfx_swtchn);
        }
        return true;

      default:
        for (i = itemOn+1;i < currentMenu->numitems;i++)
            if (currentMenu->menuitems[i].alphaKey == ch)
            {
                itemOn = i;
                S_StartSound(NULL,sfx_pstop);
                return true;
            }
        for (i = 0;i <= itemOn;i++)
            if (currentMenu->menuitems[i].alphaKey == ch)
            {
                itemOn = i;
                S_StartSound(NULL,sfx_pstop);
                return true;
            }
        break;

    }

    return false;
}



//
// M_StartControlPanel
//
void M_StartControlPanel (void)
{
    // intro might call this repeatedly
    if (menuactive)
        return;

    menuactive = 1;
    currentMenu = &MainDef;         // JDC
    itemOn = currentMenu->lastOn;   // JDC
}


//
// M_Drawer
// Called after the view has been rendered,
// but before it has been blitted.
//
void M_Drawer (void)
{
    static short        x;
    static short        y;
    short               i;
    short               max;
    char                string[40];
    int                 start;

    inhelpscreens = false;


    // Horiz. & Vertically center string and print it.
    if (messageToPrint)
    {
        start = 0;
        y = (SCREENHEIGHT/2) - M_StringHeight(messageString)/2;
        while(*(messageString+start))
        {
            for (i = 0;i < strlen(messageString+start);i++)
                if (*(messageString+start+i) == '\n')
                {
                    memset(string,0,40);
                    strncpy(string,messageString+start,i);
                    start += i+1;
                    break;
                }

            if (i == strlen(messageString+start))
            {
                strcpy(string,messageString+start);
                start += i;
            }

            x = 160 - M_StringWidth(string)/2;
            M_WriteText(x,y,string);
            y += SHORT(hu_font[0]->height);
        }
        return;
    }

    if (!menuactive)
        return;

    if (currentMenu->routine)
        currentMenu->routine();         // call Draw routine

    // DRAW MENU
    x = currentMenu->x;
    y = currentMenu->y;
    max = currentMenu->numitems;

    for (i=0;i<max;i++)
    {
        if (currentMenu->menuitems[i].name[0])
            V_DrawPatchDirect (x,y,0,
                               W_CacheLumpName(currentMenu->menuitems[i].name ,PU_CACHE));
        y += LINEHEIGHT;
    }


    // DRAW SKULL
    V_DrawPatchDirect(x + SKULLXOFF,currentMenu->y - 5 + itemOn*LINEHEIGHT, 0,
                      W_CacheLumpName(skullName[whichSkull],PU_CACHE));

}


//
// M_ClearMenus
//
void M_ClearMenus (void)
{
    menuactive = 0;
    // if (!netgame && usergame && paused)
    //       sendpause = true;
}




//
// M_SetupNextMenu
//
void M_SetupNextMenu(menu_t *menudef)
{
    currentMenu = menudef;
    itemOn = currentMenu->lastOn;
}


//
// M_Ticker
//
void M_Ticker (void)
{
    if (--skullAnimCounter <= 0)
    {
        whichSkull ^= 1;
        skullAnimCounter = 8;
    }
}


//
// M_Init
//
void M_Init (void)
{
    /* Offset all menu Y positions for taller screens */
    MainDef.y += MENU_YOFF;
    LinkDef.y += MENU_YOFF;
    EpiDef.y += MENU_YOFF;
    NewDef.y += MENU_YOFF;
    OptionsDef.y += MENU_YOFF;
    ReadDef1.y += MENU_YOFF;
    ReadDef2.y += MENU_YOFF;
    SoundDef.y += MENU_YOFF;
    LoadDef.y += MENU_YOFF;
    SaveDef.y += MENU_YOFF;

    currentMenu = &MainDef;
    menuactive = 0;
    itemOn = currentMenu->lastOn;
    whichSkull = 0;
    skullAnimCounter = 10;
    screenSize = screenblocks - 3;
    messageToPrint = 0;
    messageString = NULL;
    messageLastMenuActive = menuactive;
    quickSaveSlot = -1;

    // Here we could catch other version dependencies,
    //  like HELP1/2, and four episodes.


    switch ( gamemode )
    {
      case commercial:
        // This is used because DOOM 2 had only one HELP
        //  page. I use CREDIT as second page now, but
        //  kept this hack for educational purposes.
        MainDef.numitems--;
        MainDef.y += 8;
        NewDef.prevMenu = &MainDef;
        ReadDef1.routine = M_DrawReadThis1;
        ReadDef1.x = 330;
        ReadDef1.y = 165;
        ReadMenu1[0].routine = M_FinishReadThis;
        break;
      case shareware:
        // Episode 2 and 3 are handled,
        //  branching to an ad screen.
      case registered:
        // We need to remove the fourth episode.
        EpiDef.numitems--;
        break;
      case retail:
        // We are fine.
      default:
        break;
    }

}

