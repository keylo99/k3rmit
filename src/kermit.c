/*
 * kermit, a VTE-based, simple and froggy terminal emulator.
 * Copyright © 2019-2024 by Orhun Parmaksız <orhunparmaksiz@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "kermit.h"

#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
#include <vte/vte.h>

#define UNUSED(x) (void)(x)
#define CLR_R(x) (((x)&0xff0000) >> 16)
#define CLR_G(x) (((x)&0x00ff00) >> 8)
#define CLR_B(x) (((x)&0x0000ff) >> 0)
#define CLR_16(x) ((double)(x) / 0xff)
#define CLR_GDK(x, a)                            \
    (const GdkRGBA) { .red = CLR_16(CLR_R(x)),   \
                      .green = CLR_16(CLR_G(x)), \
                      .blue = CLR_16(CLR_B(x)),  \
                      .alpha = a }

static GtkWidget *window;                            /* Main window widget */
static GtkWidget *paned;                             /* Paned widged for the tab feature */
static GtkWidget *notebook;                          /* Notebook widget for the tab feature */
static GtkWidget *tabLabel;                          /* Label widget for the tab feature */
static PangoFontDescription *fontDesc;               /* Description for the terminal font */
static FILE *configFile;                             /* Terminal configuration file */
static float termOpacity = TERM_OPACITY;             /* Default opacity value */
static int defaultFontSize = TERM_FONT_DEFAULT_SIZE; /* Terminal font size */
static int termBackground = TERM_BACKGROUND;         /* Background color */
static int termForeground = TERM_FOREGROUND;         /* Foreground color */
static int termBoldColor = TERM_BOLD_COLOR;          /* Foreground bold color */
static int termCursorColor = TERM_CURSOR_COLOR;      /* Cursor color */
static int termCursorFg = TERM_CURSOR_FG;            /* Cursor foreground color */
static int termCursorShape = VTE_CURSOR_SHAPE_BLOCK; /* Cursor shape*/
static int currentFontSize;                          /* Necessary for changing font size */
static int keyState;                                 /* State of key press events */
static int actionKey = GDK_MOD1_MASK;                /* Key to check on press */
static int tabPosition = 0;                          /* Tab position (0/1 -> bottom/top) */
static int keyCount = 0;                             /* Count of custom binding keys */
static int colorCount = 0;                           /* Parsed color count */
static int opt;                                      /* Argument parsing option */
static char *termFont = TERM_FONT;                   /* Default terminal font */
static char *termLocale = TERM_LOCALE;               /* Terminal locale (numeric) */
static char *termWordChars = TERM_WORD_CHARS;        /* Word characters exceptions */
static char *termTitle;                              /* Title to set in terminal (-t) */
static char *wordChars;                              /* Variables for parsing the config */
static char *fontSize;
static char *configFileName; /* Configuration file name */
static char *workingDir;     /* Working directory */
static char *termCommand;    /* Command to execute in terminal (-e) */
static char *tabLabelText;   /* The label text for showing the tabs situation */
static gchar **envp;         /* Variables for starting the terminal */
static gchar **command;
static gboolean defaultConfigFile = TRUE; /* Boolean value for -c argument */
static gboolean debugMessages = FALSE;    /* Boolean value for -d argument */
static gboolean closeTab = FALSE;         /* Close the tab on child-exited signal */
static va_list vargs;                     /* Hold information about variable arguments */
static GdkRGBA termPalette[TERM_PALETTE_SIZE];   /* Terminal colors */
static char **args;                       /* Save args the terminal was launched with*/
typedef struct KeyBindings {              /* Key bindings struct */
    gboolean internal;
    char *key;
    char *cmd;
} Bindings;
typedef struct {       /* Default key bindings struct */
    Bindings bind;
    gboolean invalid;
} DefaultBindings;
static Bindings keyBindings[TERM_CONFIG_LENGTH]; /* Array for custom key bindings */
static DefaultBindings defaultKeyBindings[] = {     /* Preconfigured default key bindings */
    { .bind = { .key = "c", .cmd = "copy", .internal = TRUE } },
    { .bind = { .key = "v", .cmd = "paste", .internal = TRUE } },
    { .bind = { .key = "t", .cmd = "new-tab", .internal = TRUE } },
    { .bind = { .key = "n", .cmd = "new-window", .internal = TRUE } },
    { .bind = { .key = "return", .cmd = "new-tab", .internal = TRUE } },
    { .bind = { .key = "r", .cmd = "reload-config", .internal = TRUE } },
    { .bind = { .key = "d", .cmd = "default-config", .internal = TRUE } },
    { .bind = { .key = "q", .cmd = "exit", .internal = TRUE } },
    { .bind = { .key = "k", .cmd = "inc-font-size", .internal = TRUE } },
    { .bind = { .key = "up", .cmd = "inc-font-size", .internal = TRUE } },
    { .bind = { .key = "j", .cmd = "dec-font-size", .internal = TRUE } },
    { .bind = { .key = "down", .cmd = "dec-font-size", .internal = TRUE } },
    { .bind = { .key = "equals", .cmd = "default-font-size", .internal = TRUE } },
    { .bind = { .key = "plus", .cmd = "default-font-size", .internal = TRUE } },
    { .bind = { .key = "l", .cmd = "next-tab", .internal = TRUE } },
    { .bind = { .key = "right", .cmd = "next-tab", .internal = TRUE } },
    { .bind = { .key = "page_down", .cmd = "next-tab", .internal = TRUE } },
    { .bind = { .key = "h", .cmd = "prev-tab", .internal = TRUE } },
    { .bind = { .key = "left", .cmd = "prev-tab", .internal = TRUE } },
    { .bind = { .key = "page_up", .cmd = "prev-tab", .internal = TRUE } },
    { .bind = { .key = "w", .cmd = "close-tab", .internal = TRUE } },
    { .bind = { .key = "backspace", .cmd = "close-tab", .internal = TRUE } },
};
static size_t defaultKeyCount = sizeof(defaultKeyBindings) / sizeof(DefaultBindings);

/*!
 * Print log (debug) message with format specifiers.
 *
 * \param format string and format specifiers for vfprintf function
 * \return 0 on success
 */
static int printLog(char *format, ...) {
    if (!debugMessages) {
        return 0;
    }
    fprintf(stderr, "%s[ %sdebug%s ] ",
            TERM_ATTR_BOLD,     /* Bold on */
            TERM_ATTR_COLOR,    /* Light blue */
            TERM_ATTR_DEFAULT); /* Default color */
                                /* Format the string & print */
    va_start(vargs, format);
    vfprintf(stderr, format, vargs);
    va_end(vargs);
    /* All attributes off */
    fprintf(stderr, "%s", TERM_ATTR_OFF);
    return 0;
}

/*!
 * Set signals for terminal.
 *
 * \param terminal
 * \return 0 on success
 */
static int connectSignals(GtkWidget *terminal) {
    g_signal_connect(terminal, "child-exited", G_CALLBACK(termOnChildExit), NULL);
    g_signal_connect(terminal, "key-press-event", G_CALLBACK(termOnKeyPress), NULL);
    g_signal_connect(terminal, "window-title-changed", G_CALLBACK(termOnTitleChanged),
                     GTK_WINDOW(window));
    return 0;
}

/*!
 * Opens a new window with the same working directory
 *
 * \param terminal
 */
static void termClone(VteTerminal *terminal) {
    const char prefix[] = "file://";
    const char *path = vte_terminal_get_current_directory_uri(terminal);
    if (path && strncmp(prefix, path, strlen(prefix)) == 0) {
        path += strlen(prefix);
    }
    if (fork() == 0) {
	if (!path) {
            fprintf(stderr, "Unable to fetch current working directory\n");
	} else if (chdir(path) == -1) {
	    perror("Unable to change pwd for new terminal");
        }
        if (execvp(args[0], args) == -1) {
            perror("Cloning the terminal failed");
            _exit(errno);
        }
    }
}

/*
 * Handle internal action
 *
 * \param terminal
 * \param action
 * \return FALSE if no action is found & TRUE otherwise
 */
static gboolean termAction(GtkWidget *terminal, const char *action) {
    if (strcmp(action, "copy") == 0) {
        vte_terminal_copy_clipboard_format(VTE_TERMINAL(terminal),
                                           VTE_FORMAT_TEXT);
    } else if (strcmp(action, "paste") == 0) {
        vte_terminal_paste_clipboard(VTE_TERMINAL(terminal));
    } else if (strcmp(action, "reload-config") == 0) {
        printLog("Reloading configuration file...\n");
        if (defaultConfigFile)
            configFileName = NULL;
        parseSettings();
        configureTerm(terminal);
    } else if (strcmp(action, "default-config") == 0) {
        printLog("Loading the default configuration...\n");
        colorCount = 0;
        configureTerm(terminal);
    } else if (strcmp(action, "new-tab") == 0) {
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), getTerm(), NULL);
        gtk_widget_show_all(window);
    } else if (strcmp(action, "new-window") == 0) {
        termClone(VTE_TERMINAL(terminal));
    } else if (strcmp(action, "exit") == 0) {
        gtk_main_quit();
    } else if (strcmp(action, "inc-font-size") == 0) {
        setTermFont(terminal, currentFontSize + 1);
    } else if (strcmp(action, "dec-font-size") == 0) {
        setTermFont(terminal, currentFontSize - 1);
    } else if (strcmp(action, "default-font-size") == 0) {
        setTermFont(terminal, defaultFontSize);
    } else if (strcmp(action, "next-tab") == 0) {
        gtk_notebook_next_page(GTK_NOTEBOOK(notebook));
    } else if (strcmp(action, "prev-tab") == 0) {
        gtk_notebook_prev_page(GTK_NOTEBOOK(notebook));
    } else if (strcmp(action, "close-tab") == 0) {
        if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) == 1)
            return TRUE;
        closeTab = TRUE;
        gtk_notebook_remove_page(GTK_NOTEBOOK(notebook),
                                 gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook)));
        gtk_widget_queue_draw(GTK_WIDGET(notebook));
    } else {
        return FALSE;
    }
    return TRUE;
}

/*!
 * Handle terminal exit.
 *
 * \param terminal
 * \param status
 * \param userData
 * \return TRUE on exit
 */
static gboolean termOnChildExit(VteTerminal *terminal, gint status,
                                gpointer userData) {
    /* 'child-exited' signal is emitted on both terminal exit
     * and (notebook) page deletion. Use closeTab variable
     * to solve this issue. Also, it closes the current tab on exit.
     */
    if (!closeTab) {
        /* Close the current tab */
        if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) != 1) {
            gtk_notebook_remove_page(GTK_NOTEBOOK(notebook),
                                     gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook)));
            gtk_widget_queue_draw(GTK_WIDGET(notebook));
            /* Exit the terminal */
        } else {
            gtk_main_quit();
        }
        /* Close tab */
    } else {
        closeTab = FALSE;
    }
    return TRUE;
}

/*!
 * Handle terminal key press events.
 *
 * \param terminal
 * \param event (key press or release)
 * \param userData
 * \return FALSE on normal press & TRUE on custom actions
 */
static gboolean termOnKeyPress(GtkWidget *terminal, GdkEventKey *event,
                               gpointer userData) {
    /* Unused user data */
    UNUSED(userData);
    /* Check for CTRL, ALT and SHIFT keys */
    keyState = event->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK);
    /* CTRL + binding + key */
    if (keyState == (actionKey | GDK_CONTROL_MASK)) {
        if (atoi(gdk_keyval_name(event->keyval)) != 0) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook),
                                          atoi(gdk_keyval_name(event->keyval)) - 1);
            return TRUE;
        }
        for (int i = 0; i < defaultKeyCount; i++) {
            if (!strcasecmp(gdk_keyval_name(event->keyval), defaultKeyBindings[i].bind.key)) {
                if (!defaultKeyBindings[i].invalid) {
                    if (defaultKeyBindings[i].bind.internal) {
                        termAction(terminal, defaultKeyBindings[i].bind.cmd);
                    } else {
                        vte_terminal_feed_child(VTE_TERMINAL(terminal),
                                                defaultKeyBindings[i].bind.cmd, -1);
                    }
                    return TRUE;
                }
            }
        }
        for (int i = 0; i < keyCount; i++) {
            if (!strcasecmp(gdk_keyval_name(event->keyval), keyBindings[i].key)) {
                if (keyBindings[i].internal) {
                    termAction(terminal, keyBindings[i].cmd);
                } else {
                    vte_terminal_feed_child(VTE_TERMINAL(terminal),
                                            keyBindings[i].cmd, -1);
                }
                return TRUE;
            }
        }
    }
    return FALSE;
}

/*!
 * Set the terminal title on changes.
 *
 * \param terminal
 * \param userData
 * \return TRUE on title change
 */
static gboolean termOnTitleChanged(GtkWidget *terminal, gpointer userData) {
    GtkWindow *window = userData;
    if (termTitle == NULL)
        gtk_window_set_title(window,
                             vte_terminal_get_window_title(VTE_TERMINAL(terminal)) ?: TERM_NAME);
    else
        gtk_window_set_title(window, termTitle);
    return TRUE;
}

/*!
 * Set the divider position using current window size.
 *
 * \param widget
 * \param allocation
 * \param userData
 * \return TRUE on size change
 */
static gboolean termOnResize(GtkWidget *widget, GtkAllocation *allocation,
                             gpointer userData) {
    if (tabPosition == 1)
        allocation->height = 0;
    if (GTK_PANED(userData) != NULL)
        gtk_paned_set_position(GTK_PANED(userData), allocation->height - 20);
    return TRUE;
}

/*!
 * Switch to last page when new tab added.
 *
 * \param notebook
 * \param child
 * \param pageNum
 * \param userData
 * \return TRUE on tab addition
 */
static gboolean termTabOnAdd(GtkNotebook *notebook, GtkWidget *child,
                             guint pageNum, gpointer userData) {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), pageNum);
    return TRUE;
}

/*!
 * The switch event for terminal tabs.
 *
 * \param notebook
 * \param page
 * \param pageNum
 * \param userData
 * \return TRUE on switch
 */
static gboolean termTabOnSwitch(GtkNotebook *notebook, GtkWidget *page,
                                guint pageNum, gpointer userData) {
    /* Destroy tabs label if there's not more than one tabs */
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) == 1) {
        if (tabLabel != NULL)
            gtk_widget_destroy(tabLabel);
        return TRUE;
        /* Add tabs label to paned if it doesn't exist */
    } else if (gtk_paned_get_child1(GTK_PANED(paned)) == NULL ||
               gtk_paned_get_child2(GTK_PANED(paned)) == NULL) {
        tabLabel = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(tabLabel), 0);
        if (tabPosition == 0)
            gtk_paned_add2(GTK_PANED(paned), tabLabel);
        else
            gtk_paned_add1(GTK_PANED(paned), tabLabel);
    }
    /* Same font as terminal but smaller */
    gchar *fontStr = g_strconcat(termFont, " ",
                                 g_strdup_printf("%d", defaultFontSize - 1), NULL);
    /* Prepare the label text (use different color for current tab) */
    tabLabelText = g_markup_printf_escaped(
        "<span font='\%s' foreground='#\%02X\%02X\%02X'>",
        fontStr,
        (int)(termPalette[4].red * 255),
        (int)(termPalette[4].green * 255),
        (int)(termPalette[4].blue * 255));
    for (int i = 0; i < gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)); i++) {
        if (i == pageNum)
            tabLabelText = g_strconcat(tabLabelText,
                                       g_strdup_printf("<span foreground='#%x'> %d </span>",
                                                       termForeground, i + 1),
                                       NULL);
        else
            tabLabelText = g_strconcat(tabLabelText,
                                       g_strdup_printf(" %d ", i + 1), NULL);
    }
    tabLabelText = g_strconcat(tabLabelText, "~</span>", NULL);
    /* Set the label text with markup */
    gtk_label_set_markup(GTK_LABEL(tabLabel), tabLabelText);
    g_free(fontStr);
    return TRUE;
}

/*!
 * Set the terminal font with given size.
 *
 * \param terminal
 * \param fontSize
 * \return 0 on success
 */
static int setTermFont(GtkWidget *terminal, int fontSize) {
    gchar *fontStr = g_strconcat(termFont, " ",
                                 g_strdup_printf("%d", fontSize), NULL);
    if ((fontDesc = pango_font_description_from_string(fontStr)) != NULL) {
        vte_terminal_set_font(VTE_TERMINAL(terminal), fontDesc);
        currentFontSize = fontSize;
        pango_font_description_free(fontDesc);
        g_free(fontStr);
    }
    return 0;
}

/*!
 * Update the palette and set terminal colors.
 *
 * \param terminal
 * \return 0 on success
 */
static int setTermColors(GtkWidget *terminal) {
    for (int i = colorCount; i < 256; i++) {
        if (i < 16) {
            termPalette[i].blue = (((i & 4) ? 0xc000 : 0) + (i > 7 ? 0x3fff : 0)) / 65535.0;
            termPalette[i].green = (((i & 2) ? 0xc000 : 0) + (i > 7 ? 0x3fff : 0)) / 65535.0;
            termPalette[i].red = (((i & 1) ? 0xc000 : 0) + (i > 7 ? 0x3fff : 0)) / 65535.0;
            termPalette[i].alpha = 0;
        } else if (i < 232) {
            const unsigned j = i - 16;
            const unsigned r = j / 36, g = (j / 6) % 6, b = j % 6;
            termPalette[i].red = ((r == 0) ? 0 : r * 40 + 55) / 255.0;
            termPalette[i].green = ((g == 0) ? 0 : g * 40 + 55) / 255.0;
            termPalette[i].blue = ((b == 0) ? 0 : b * 40 + 55) / 255.0;
            termPalette[i].alpha = 0;
        } else if (i < 256) {
            const unsigned shade = 8 + (i - 232) * 10;
            termPalette[i].red = termPalette[i].green =
                termPalette[i].blue = (shade | shade << 8) / 65535.0;
            termPalette[i].alpha = 0;
        }
    }
    vte_terminal_set_colors(VTE_TERMINAL(terminal),
                            &CLR_GDK(termForeground, 0),           /* Foreground */
                            &CLR_GDK(termBackground, termOpacity), /* Background */
                            termPalette,                           /* Palette */
                            sizeof(termPalette) / sizeof(GdkRGBA));
    vte_terminal_set_color_bold(VTE_TERMINAL(terminal),
                                &CLR_GDK(termBoldColor, 0));
    return 0;
}

/*!
 * Configure the terminal.
 *
 * \param terminal
 * \return 0 on success
 */
static int configureTerm(GtkWidget *terminal) {
    /* Set numeric locale */
    setlocale(LC_NUMERIC, termLocale);
    /* Hide the mouse cursor when typing */
    vte_terminal_set_mouse_autohide(VTE_TERMINAL(terminal), TRUE);
    /* Scroll issues */
    vte_terminal_set_scroll_on_output(VTE_TERMINAL(terminal), FALSE);
    vte_terminal_set_scroll_on_keystroke(VTE_TERMINAL(terminal), TRUE);
    vte_terminal_set_scrollback_lines(VTE_TERMINAL(terminal), -1);
    /* Rewrap the content when terminal size changed */
    vte_terminal_set_rewrap_on_resize(VTE_TERMINAL(terminal), TRUE);
    /* Disable audible bell */
    vte_terminal_set_audible_bell(VTE_TERMINAL(terminal), FALSE);
    /* Enable bold text */
    vte_terminal_set_allow_bold(VTE_TERMINAL(terminal), TRUE);
    /* Allow hyperlinks */
    vte_terminal_set_allow_hyperlink(VTE_TERMINAL(terminal), TRUE);
    /* Set char exceptions */
    vte_terminal_set_word_char_exceptions(VTE_TERMINAL(terminal),
                                          termWordChars);
    /* Zuckerberg feature */
    vte_terminal_set_cursor_blink_mode(VTE_TERMINAL(terminal),
                                       VTE_CURSOR_BLINK_OFF);
    /* Set cursor options */
    vte_terminal_set_color_cursor(VTE_TERMINAL(terminal),
                                  &CLR_GDK(termCursorColor, 0));
    vte_terminal_set_color_cursor_foreground(VTE_TERMINAL(terminal),
                                             &CLR_GDK(termCursorFg, 0));
    vte_terminal_set_cursor_shape(VTE_TERMINAL(terminal), termCursorShape);
    /* Set the terminal colors and font */
    setTermColors(terminal);
    setTermFont(terminal, defaultFontSize);
    return 0;
}

/*!
 * Async callback for terminal state.
 *
 * \param terminal
 * \param pid (Process ID)
 * \param error
 * \param userData
 */
static void termStateCallback(VteTerminal *terminal, GPid pid,
                              GError *error, gpointer userData) {
    if (error == NULL) {
        printLog("%s started. (PID: %d)\n", TERM_NAME, pid);
    } else {
        printLog("An error occurred: %s\n", error->message);
        g_clear_error(&error);
    }
    UNUSED(userData);
    UNUSED(terminal);
}

/*!
 * Create a new terminal widget with a shell.
 *
 * \return terminal
 */
static GtkWidget *getTerm() {
    /* Create a terminal widget */
    GtkWidget *terminal = vte_terminal_new();
    /* Terminal configuration */
    connectSignals(terminal);
    configureTerm(terminal);
    /* Start a new shell */
    envp = g_get_environ();
    command = (gchar *[]){g_strdup(g_environ_getenv(envp, "SHELL")), NULL};
    printLog("shell: %s\n", *command);
    if (termCommand != NULL) {
        command = (gchar *[]){g_strdup(g_environ_getenv(envp, "SHELL")),
                              "-c", termCommand, NULL};
        printLog("command: %s %s %s\n", command[0], command[1], command[2]);
    }
    g_strfreev(envp);
    if (workingDir == NULL) {
        workingDir = g_get_current_dir();
    }
    printLog("workdir: %s\n", g_get_current_dir());
    /* Spawn terminal asynchronously */
    vte_terminal_spawn_async(VTE_TERMINAL(terminal),
                             VTE_PTY_DEFAULT,   /* pty flag */
                             workingDir,        /* working directory */
                             command,           /* argv */
                             NULL,              /* environment variables */
                             G_SPAWN_DEFAULT,   /* spawn flag */
                             NULL,              /* child setup function */
                             NULL,              /* child setup data */
                             NULL,              /* child setup data destroy */
                             -1,                /* timeout */
                             NULL,              /* cancellable */
                             termStateCallback, /* async callback */
                             NULL);             /* callback data */
    /* Show the terminal widget */
    gtk_widget_show(terminal);
    return terminal;
}

/*!
 * Invalidate the default binding with the corresponding action
 *
 * \param binding
 */
static void invalidateDefaultBinding(const Bindings *binding) {
    for (int i = 0; i < defaultKeyCount; i++) {
        if (strcmp(defaultKeyBindings[i].bind.cmd, binding->cmd) == 0
            && defaultKeyBindings[i].bind.internal == binding->internal) {
            defaultKeyBindings[i].invalid = TRUE;
        }
    }
}

/*!
 * Initialize and start the terminal.
 *
 * \return 0 on success
 */
static int startTerm() {
    /* Create & configure the window widget */
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    if (termTitle == NULL)
        gtk_window_set_title(GTK_WINDOW(window), TERM_NAME);
    else
        gtk_window_set_title(GTK_WINDOW(window), termTitle);
    gtk_widget_set_visual(window, /* Alpha channel for transparency */
                          gdk_screen_get_rgba_visual(gtk_widget_get_screen(window)));
    gtk_widget_override_background_color(window, GTK_STATE_FLAG_NORMAL,
                                         &CLR_GDK(termBackground, termOpacity));
    /* Create & configure the paned widget */
    paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_wide_handle(GTK_PANED(paned), FALSE);
    /* Create & configure the notebook widget */
    notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_BOTTOM);
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_notebook_popup_disable(GTK_NOTEBOOK(notebook));
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
    /* Connect signals of window and notebook for tab feature */
    g_signal_connect(window, "delete-event", gtk_main_quit, NULL);
    g_signal_connect(window, "size-allocate", G_CALLBACK(termOnResize), paned);
    g_signal_connect(notebook, "page-added", G_CALLBACK(termTabOnAdd), NULL);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(termTabOnSwitch), NULL);
    /* Add terminal to notebook as first tab */
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), getTerm(), NULL);
    /* Add notebook to paned */
    if (tabPosition == 0)
        gtk_paned_add1(GTK_PANED(paned), notebook);
    else
        gtk_paned_add2(GTK_PANED(paned), notebook);
    /* Add paned to main window */
    gtk_container_add(GTK_CONTAINER(window), paned);
    /* Show all widgets with childs and run the main loop */
    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}

/*!
 * Parse the color value.
 *
 * \param value
 * \return color
 */
static int parseColor(char *value) {
    if (value[0] == '#') {
        memmove(value, value + 1, strlen(value));
        sprintf(value, "%s", g_strconcat("0x", value, NULL));
    }
    return (int)strtol(value, NULL, 16);
}

/*!
 * Read settings from configuration file and apply.
 */
static void parseSettings() {
    char buf[TERM_CONFIG_LENGTH],
        option[TERM_CONFIG_LENGTH],
        value[TERM_CONFIG_LENGTH];
    if (configFileName == NULL)
        configFileName = g_strconcat(getenv("HOME"),
                                     TERM_CONFIG_DIR, TERM_NAME, ".conf", NULL);
    else
        defaultConfigFile = FALSE;
    configFile = fopen(configFileName, "r");
    if (configFile == NULL) {
        printLog("config file not found. (%s)\n", configFileName);
        return;
    }
    keyCount = 0;
    while (fgets(buf, TERM_CONFIG_LENGTH, configFile)) {
        /* Skip lines starting with '#' and invalid lines */
        if (buf[0] == '#' || strlen(buf) < 4)
            continue;
        /* Get option and value from line */
        sscanf(buf, "%s %s\n", option, value);
        /* Locale */
        if (!strncmp(option, "locale", strlen(option))) {
            termLocale = g_strdup(value);
            /* Word chars */
        } else if (!strncmp(option, "char", strlen(option))) {
            /* Remove '"' from word chars */
            wordChars = g_strdup(value);
            wordChars[strlen(wordChars) - 1] = 0;
            termWordChars = wordChars + 1;
            /* Action key */
        } else if (!strncmp(option, "key", strlen(option))) {
            if (!strncmp(value, "alt", strlen(value)))
                actionKey = GDK_MOD1_MASK;
            else
                actionKey = GDK_SHIFT_MASK;
            /* Key bindings */
        } else if (!strncmp(option, "bind", strlen(option) - 1)) {
            /* Parse the line again for command */
            sscanf(buf, "%s %[^\n]\n", option, value);
            /* Split the line and the values */
            char *key = strtok(value, "~");
            char *cmd = strtok(NULL, "");
            if (cmd != NULL) {
                /* Trim and add to the commands */
                cmd[strlen(cmd) - 1] = 0;
                /* Execute option is provided */
                if (!strcmp(option, "bindx"))
                    /* Append carriage return to command */
                    keyBindings[keyCount].cmd =
                        g_strconcat(g_strdup(cmd + 1), "\r", NULL);
                else
                    keyBindings[keyCount].cmd = g_strdup(cmd + 1);
                /* Internal option is specified */
                if (!strcmp(option, "bindi"))
                    /* Set binding as internal*/
                    keyBindings[keyCount].internal = TRUE;
                else
                    keyBindings[keyCount].internal = FALSE;
                /* Add key binding to the keys */
                keyBindings[keyCount].key = g_strdup(key);
                printLog("cmd %d = %s -> \"%s\"\n", keyCount + 1,
                         keyBindings[keyCount].key,
                         keyBindings[keyCount].cmd);
                /* Invalidate associated default bindings */
                invalidateDefaultBinding(&keyBindings[keyCount]);
                /* Increment the keys count */
                keyCount++;
            }
            /* Tab position */
        } else if (!strncmp(option, "tab", strlen(option))) {
            if (!strncmp(value, "bottom", strlen(value)))
                tabPosition = 0;
            else
                tabPosition = 1;
            /* Terminal font */
        } else if (!strncmp(option, "font", strlen(option))) {
            /* Parse the line again for font size */
            sscanf(buf, "%s %[^\n]\n", option, value);
            /* Split the line and get last element */
            fontSize = strrchr(value, ' ');
            if (fontSize != NULL) {
                /* Trim and set the font size */
                defaultFontSize = atoi(fontSize + 1);
                /* Get the font information excluding font size */
                *fontSize = 0;
                termFont = g_strdup(value);
            }
            /* Opacity value */
        } else if (!strncmp(option, "opacity", strlen(option))) {
            termOpacity = atof(value);
            /* Cursor colors */
        } else if (!strncmp(option, "cursor", strlen(option))) {
            termCursorColor = parseColor(value);
        } else if (!strncmp(option, "cursor_foreground", strlen(option))) {
            termCursorFg = parseColor(value);
            /* Cursor shape */
        } else if (!strncmp(option, "cursor_shape", strlen(option))) {
            if (!strncmp(value, "underline", strlen(value)))
                termCursorShape = VTE_CURSOR_SHAPE_UNDERLINE;
            else if (!strncmp(value, "ibeam", strlen(value)))
                termCursorShape = VTE_CURSOR_SHAPE_IBEAM;
            else
                termCursorShape = VTE_CURSOR_SHAPE_BLOCK;
            /* Foreground color */
        } else if (!strncmp(option, "foreground", strlen(option))) {
            termForeground = parseColor(value);
            /* Bold color */
        } else if (!strncmp(option, "foreground_bold", strlen(option))) {
            termBoldColor = parseColor(value);
            /* Background color */
        } else if (!strncmp(option, "background", strlen(option))) {
            termBackground = parseColor(value);
            /* Color palette */
        } else if (!strncmp(option, "color", strlen(option) - 2)) {
            /* Get the color index */
            char *colorIndex = strrchr(option, 'r');
            if (colorIndex != NULL) {
                /* Set the color in palette */
                termPalette[atoi(colorIndex + 1)] =
                    CLR_GDK(parseColor(value), 0);
                colorCount++;
            }
        }
    }
    fclose(configFile);
    if (defaultConfigFile)
        g_free(configFileName);
}

/*!
 * Parse command line arguments.
 *
 * \param argc (argument count)
 * \param argv (argument vector)
 * \return 1 on exit
 */
static int parseArgs(int argc, char **argv) {
    args = argv;
    while ((opt = getopt(argc, argv, ":c:w:e:t:vdh")) != -1) {
        switch (opt) {
            case 'c':
                /* Configuration file name to read */
                configFileName = optarg;
                break;
            case 'w':
                /* Working directory */
                workingDir = optarg;
                break;
            case 'e':
                /* Command to execute in terminal */
                termCommand = optarg;
                break;
            case 't':
                /* Title to set in terminal */
                termTitle = optarg;
                break;
            case 'd':
                /* Activate debug messages */
                debugMessages = TRUE;
                break;
            case 'v':
                /* Show version information */
                fprintf(stderr,
                        "%s   (+)(+)\n"
                        "  /      \\\n"
                        "  \\ -==- /\n"
                        "   \\    /\n"
                        "  <\\/\\/\\/>\n"
                        "  /      \\\n"
                        " [ %skermit%s ] ~ v%s%s\n",
                        TERM_ATTR_BOLD, TERM_ATTR_COLOR,
                        TERM_ATTR_DEFAULT, TERM_VERSION,
                        TERM_ATTR_OFF);
                return 1;
            case 'h': /* Fallthrough */
            case '?':
                /* Show help message */
                fprintf(stderr,
                        "%s[ %susage%s ] %s [-h] "
                        "[-v] [-d] [-c config] [-t title] [-w workdir] [-e command]%s\n",
                        TERM_ATTR_BOLD,
                        TERM_ATTR_COLOR,
                        TERM_ATTR_DEFAULT,
                        TERM_NAME,
                        TERM_ATTR_OFF);
                return 1;
            case ':':
                /* Show debug message on missing argument */
                debugMessages = TRUE;
                printLog("Option requires an argument.\n");
                return 1;
        }
    }
    return 0;
}

/*!
 * Entry-point
 */
int main(int argc, char *argv[]) {
    /* Parse command line arguments */
    if (parseArgs(argc, argv))
        return 0;
    /* Parse settings if configuration file exists */
    parseSettings();
    /* Initialize GTK and start the terminal */
    gtk_init(&argc, &argv);
    startTerm();
    return 0;
}
