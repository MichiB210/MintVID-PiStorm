#include "mr_gui_menu.h"
#include "mr_p96_format.h"
#include "mr_tls_pref.h"
#include "mr_paula_pref.h"

#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <string.h>
#include <stdio.h>

struct Library *GadToolsBase;

/* Stack for the "AmigaGuide" process mr_gui_open_guide() launches - a
 * lightweight system utility, unlike mrplay's $STACK:320000 (libavc H.264
 * needs), so a modest size well above the Shell's own ~4-8 KB default is
 * plenty. */
#define MR_GUIDE_STACK_SIZE 20000UL

#define MR_MENU_USER_GUIDE ((APTR)1)
#define MR_MENU_USER_ABOUT ((APTR)2)
#define MR_MENU_USER_QUIT  ((APTR)3)
#define MR_MENU_USER_P96_YVYU ((APTR)4)
#define MR_MENU_USER_P96_YUYV ((APTR)5)
#define MR_MENU_USER_TLS_12 ((APTR)6)
#define MR_MENU_USER_TLS_13 ((APTR)7)
/* P96W73: Paula base offset items, user data 100 + index (0 .. -1000 ms). */
#define MR_MENU_USER_PAULA0 100
#define MR_PAULA_STEPS 21
int mr_gui_menu_with_paula;          /* set by MintVID-pi before open */
static char paula_labels[MR_PAULA_STEPS][16];
static struct NewMenu paula_menu[40];
/* Indices in menu_template: title, Guide, About, P96 parent, two subs,
 * HTTPS parent, two subs. */
#define MR_P96_YVYU_ITEM 4
#define MR_P96_YUYV_ITEM 5
#define MR_TLS_12_ITEM 7
#define MR_TLS_13_ITEM 8

static struct NewMenu menu_template[] = {
    {NM_TITLE, (STRPTR)"MintVID", NULL, 0, 0, NULL},
    {NM_ITEM, (STRPTR)"Guide...", (STRPTR)"H", 0, 0, MR_MENU_USER_GUIDE},
    {NM_ITEM, (STRPTR)"About MintVID...", (STRPTR)"?", 0, 0,
     MR_MENU_USER_ABOUT},
    {NM_ITEM, (STRPTR)"P96 Output Format", NULL, 0, 0, NULL},
    {NM_SUB, (STRPTR)"YVYU (WinUAE)", NULL, CHECKIT | MENUTOGGLE,
     1UL << 1, MR_MENU_USER_P96_YVYU},
    {NM_SUB, (STRPTR)"YUYV (Voodoo)", NULL, CHECKIT | MENUTOGGLE,
     1UL << 0, MR_MENU_USER_P96_YUYV},
    {NM_ITEM, (STRPTR)"HTTPS", NULL, 0, 0, NULL},
    {NM_SUB, (STRPTR)"TLS 1.2 (faster)", NULL, CHECKIT | MENUTOGGLE,
     1UL << 1, MR_MENU_USER_TLS_12},
    {NM_SUB, (STRPTR)"TLS 1.3", NULL, CHECKIT | MENUTOGGLE,
     1UL << 0, MR_MENU_USER_TLS_13},
    {NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL},
    {NM_ITEM, (STRPTR)"Quit", (STRPTR)"Q", 0, 0, MR_MENU_USER_QUIT},
    {NM_END, NULL, NULL, 0, 0, NULL}
};

int mr_gui_menu_open(mr_gui_menu *menu, struct Window *window)
{
    if (!menu || !window || !window->WScreen)
        return 0;
    memset(menu, 0, sizeof(*menu));
    if (!GadToolsBase) {
        GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37);
        menu->owns_gadtools = GadToolsBase != NULL;
    }
    if (!GadToolsBase)
        return 0;
    /* Restore the persisted mutually-exclusive selection on every open. */
    {
        int yuyv = mr_p96_format_load();
        menu_template[MR_P96_YVYU_ITEM].nm_Flags =
            CHECKIT | MENUTOGGLE | (yuyv ? 0 : CHECKED);
        menu_template[MR_P96_YUYV_ITEM].nm_Flags =
            CHECKIT | MENUTOGGLE | (yuyv ? CHECKED : 0);
    }
    {
        int allow13 = mr_tls_pref_load_allow13();
        menu_template[MR_TLS_12_ITEM].nm_Flags =
            CHECKIT | MENUTOGGLE | (allow13 ? 0 : CHECKED);
        menu_template[MR_TLS_13_ITEM].nm_Flags =
            CHECKIT | MENUTOGGLE | (allow13 ? CHECKED : 0);
    }
    menu->visual_info = GetVisualInfoA(window->WScreen, NULL);
    if (!menu->visual_info)
        goto fail;
    if (mr_gui_menu_with_paula) {
        /* P96W73 (MintVID-pi): menu_template up to the bar before Quit,
         * then "Paula sound base" with 0 .. -1000 ms in 50-ms steps, then
         * the bar and Quit. */
        int n = 0, i, bar;
        long cur = mr_paula_pref_load();
        ULONG all = (1UL << MR_PAULA_STEPS) - 1UL;
        for (bar = 0; menu_template[bar].nm_Type != NM_END &&
                      menu_template[bar].nm_Label != NM_BARLABEL; bar++)
            ;
        for (i = 0; i < bar; i++) {
            /* P96W74: MintVID-pi plays no network streams - no HTTPS. */
            if (i >= MR_TLS_12_ITEM - 1 && i <= MR_TLS_13_ITEM) continue;
            /* P96W75: nor the P96 overlay byte order (graphics-card
             * playback of the classic MintVID, not used by the Pi). */
            if (i >= MR_P96_YVYU_ITEM - 1 && i <= MR_P96_YUYV_ITEM) continue;
            paula_menu[n] = menu_template[i];
            if (menu_template[i].nm_UserData == MR_MENU_USER_ABOUT)
                paula_menu[n].nm_Label = (STRPTR)"About MintVID-pi...";
            n++;
        }
        memset(&paula_menu[n], 0, sizeof paula_menu[n]);
        paula_menu[n].nm_Type = NM_ITEM;
        paula_menu[n].nm_Label = (STRPTR)"Paula sound base";
        n++;
        for (i = 0; i < MR_PAULA_STEPS; i++) {
            long ms = -50L * i;
            snprintf(paula_labels[i], sizeof paula_labels[i], "%ld ms", ms);
            memset(&paula_menu[n], 0, sizeof paula_menu[n]);
            paula_menu[n].nm_Type = NM_SUB;
            paula_menu[n].nm_Label = (STRPTR)paula_labels[i];
            paula_menu[n].nm_Flags = CHECKIT | MENUTOGGLE | (ms == cur ? CHECKED : 0);
            paula_menu[n].nm_MutualExclude = all & ~(1UL << i);
            paula_menu[n].nm_UserData = (APTR)(ULONG)(MR_MENU_USER_PAULA0 + i);
            n++;
        }
        for (i = bar; menu_template[i].nm_Type != NM_END; i++)
            paula_menu[n++] = menu_template[i];
        memset(&paula_menu[n], 0, sizeof paula_menu[n]);
        paula_menu[n].nm_Type = NM_END;
        menu->strip = CreateMenusA(paula_menu, NULL);
    } else
    menu->strip = CreateMenusA(menu_template, NULL);
    if (!menu->strip ||
        !LayoutMenusA(menu->strip, menu->visual_info, NULL))
        goto fail;
    SetMenuStrip(window, menu->strip);
    return 1;

fail:
    mr_gui_menu_close(menu, window);
    return 0;
}

void mr_gui_menu_close(mr_gui_menu *menu, struct Window *window)
{
    if (!menu)
        return;
    if (window && menu->strip)
        ClearMenuStrip(window);
    if (menu->strip) {
        FreeMenus(menu->strip);
        menu->strip = NULL;
    }
    if (menu->visual_info) {
        FreeVisualInfo(menu->visual_info);
        menu->visual_info = NULL;
    }
    if (GadToolsBase && menu->owns_gadtools) {
        CloseLibrary(GadToolsBase);
        GadToolsBase = NULL;
    }
    menu->owns_gadtools = 0;
}

int mr_gui_menu_action(mr_gui_menu *menu, UWORD code)
{
    struct MenuItem *item;
    APTR user_data;

    if (!menu || !menu->strip || code == MENUNULL)
        return MR_GUI_MENU_NONE;
    item = ItemAddress(menu->strip, code);
    if (!item)
        return MR_GUI_MENU_NONE;
    user_data = GTMENUITEM_USERDATA(item);
    if (user_data == MR_MENU_USER_P96_YVYU ||
        user_data == MR_MENU_USER_P96_YUYV) {
        int yuyv = user_data == MR_MENU_USER_P96_YUYV;
        if (mr_p96_format_save(yuyv)) {
            /* Keep the selected item checked even if clicked twice. */
            item->Flags |= CHECKED;
        } else {
            printf("MintVID: could not save P96 Output Format preference\n");
        }
        return MR_GUI_MENU_NONE;
    }
    if (user_data == MR_MENU_USER_TLS_12 ||
        user_data == MR_MENU_USER_TLS_13) {
        if (mr_tls_pref_save_allow13(user_data == MR_MENU_USER_TLS_13)) {
            item->Flags |= CHECKED;
        } else {
            printf("MintVID: could not save HTTPS TLS preference\n");
        }
        /* The browsers apply it to their own fetches straight away; mrplay
         * reads it when the next stream starts. */
        return MR_GUI_MENU_TLS;
    }
    if ((ULONG)user_data >= MR_MENU_USER_PAULA0 &&
        (ULONG)user_data < MR_MENU_USER_PAULA0 + MR_PAULA_STEPS) {
        long ms = -50L * (long)((ULONG)user_data - MR_MENU_USER_PAULA0);
        if (mr_paula_pref_save(ms))
            item->Flags |= CHECKED;
        else
            printf("MintVID-pi: could not save the Paula sound base\n");
        return MR_GUI_MENU_PAULA;
    }
    if (user_data == MR_MENU_USER_ABOUT)
        return MR_GUI_MENU_ABOUT;
    if (user_data == MR_MENU_USER_QUIT)
        return MR_GUI_MENU_QUIT;
    if (user_data == MR_MENU_USER_GUIDE)
        return MR_GUI_MENU_GUIDE;
    return MR_GUI_MENU_NONE;
}

static void guide_error(struct Window *window, const char *text)
{
    struct EasyStruct request;
    request.es_StructSize = sizeof(request);
    request.es_Flags = 0;
    request.es_Title = (UBYTE *)"MintVID Guide";
    request.es_TextFormat = (UBYTE *)text;
    request.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequestArgs(window, &request, NULL, NULL);
}

/*
 * Opens PROGDIR:MintVID.guide using the standard AmigaOS AmigaGuide command.
 * The guide is first locked and resolved to an absolute path in this process;
 * a newly-created process does not inherit this process's PROGDIR: assignment.
 * The child is otherwise launched with the same proven LoadSeg() and
 * CreateNewProcTags() pattern used for mrplay/iptvgui/ytgui.
 *
 * The process runs detached and this function does not wait for it.
 */
void mr_gui_open_guide(mr_gui_menu *menu, struct Window *window)
{
    BPTR lock;
    BPTR seglist;
    struct Process *process;
    char guide_path[512];
    char arguments[520];
    int argument_length;

    (void)menu;

    lock = Lock((CONST_STRPTR)"PROGDIR:MintVID.guide", ACCESS_READ);
    if (!lock) {
        guide_error(window,
            "MintVID.guide was not found next to this program.\n"
            "Keep it in the same drawer as the MintVID binaries.");
        return;
    }
    /*
     * PROGDIR: belongs to this process. The AmigaGuide child gets its own
     * process-local PROGDIR:, so resolve the locked file while ours is still
     * valid and pass the child a real absolute path. MintPRINT uses the same
     * rule after its literal PROGDIR: command line failed on real hardware.
     */
    if (!NameFromLock(lock, (STRPTR)guide_path, sizeof(guide_path))) {
        UnLock(lock);
        guide_error(window,
            "Could not resolve the full path to MintVID.guide.\n"
            "Open the guide manually from the program drawer.");
        return;
    }
    UnLock(lock);

    /* LoadSeg() resolves a DOS path literally; it does not perform the
     * Shell's command-path search. AmigaGuide is installed in C: on a
     * standard AmigaOS system, so name that path explicitly. */
    seglist = LoadSeg((CONST_STRPTR)"C:AmigaGuide");
    if (!seglist) {
        guide_error(window,
            "The AmigaGuide command was not found (normally\n"
            "C:AmigaGuide, part of a standard AmigaOS 2.1+/3.0+\n"
            "installation). You can still read MintVID.guide with\n"
            "a text editor.");
        return;
    }

    /* The child has its own process-local PROGDIR:, so pass the absolute
     * path resolved above. Quote it so drawers containing spaces also work. */
    argument_length = snprintf(arguments, sizeof(arguments), "\"%s\"\n",
                               guide_path);
    if (argument_length < 0 ||
        argument_length >= (int)sizeof(arguments)) {
        UnLoadSeg(seglist);
        guide_error(window,
            "The full path to MintVID.guide is too long.\n"
            "Open the guide manually from the program drawer.");
        return;
    }

    process = CreateNewProcTags(
        NP_Seglist, seglist,
        NP_FreeSeglist, TRUE,
        NP_Arguments, (ULONG)arguments,
        NP_StackSize, MR_GUIDE_STACK_SIZE,
        NP_Cli, TRUE,
        NP_CommandName, (ULONG)"AmigaGuide",
        NP_Name, (ULONG)"MintVID Guide",
        TAG_END);
    if (!process) {
        UnLoadSeg(seglist);
        guide_error(window, "Could not start the AmigaGuide command.");
    }
}

void mr_gui_show_about(struct Window *window, const char *edition)
{
    struct EasyStruct request;
    const char *name = edition && *edition ? edition : "AmigaOS edition";

    if (mr_gui_menu_with_paula) {
        /* P96W74: MintVID-pi (PiStorm) About, credits as Michael wants. */
        request.es_StructSize = sizeof(request);
        request.es_Flags = 0;
        request.es_Title = (UBYTE *)"About MintVID-pi";
        request.es_TextFormat = (UBYTE *)
            "MintVID-pi - H.264 video for PiStorm (Raspberry Pi 3A+)\n"
            "Version v96.79 (27.10.2026)\n\n"
            "PiStorm player (mrberry, berryvp.library):\n"
            "H.264 on the Pi's VideoCore, AAC/AC3/E-AC3 sound\n"
            "modules on the Pi's second core, HDMI output\n"
            "by Michael 'MichiB210' (Germany)\n"
            "https://github.com/MichiB210/MintVID-PiStorm\n\n"
            "Paula sound player by Darren 'boingball' Banfi (UK)\n\n"
            "Based on MintVID " MINTVID_VERSION " by Darren 'boingball' Banfi\n"
            "https://github.com/boingball/MintVID\n\n"
            "Emu68 by Michal Schulz, PiStorm by Claude Schwarz.\n"
            "Built with MintAMP audio decoders, liba52 and Claude.";
        request.es_GadgetFormat = (UBYTE *)"Cheers!";
        EasyRequestArgs(window, &request, NULL, NULL);
        return;
    }

    request.es_StructSize = sizeof(request);
    request.es_Flags = 0;
    request.es_Title = (UBYTE *)"About MintVID";
    request.es_TextFormat = (UBYTE *)
        "MintVID " MINTVID_VERSION " - Video for classic AmigaOS\n"
        "%s\n"
        "Local files, HLS, IPTV and YouTube playback\n"
        "including WMV7/8, MPEG-1/2/4 and H.264.\n\n"
        "Made by Darren 'boingball' Banfi\n"
        "Copyright 2026\n\n"
        "Source and licence notices:\n"
        "https://github.com/boingball/MintVID\n\n"
        "Support development and the LLM token fund:\n"
        "https://buymeacoffee.com/boingball\n\n"
        "Built with libavc, libmpeg2, MintAMP audio decoders,\n"
        "liba52, Claude and Codex.";
    request.es_GadgetFormat = (UBYTE *)"Cheers!";
    EasyRequestArgs(window, &request, NULL, (APTR)&name);
}
