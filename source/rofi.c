/**
 * rofi
 *
 * MIT/X11 License
 * Copyright (c) 2012 Sean Pringle <sean.pringle@gmail.com>
 * Modified 2013-2014 Qball  Cow <qball@gmpclient.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdint.h>
#include <ctype.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <err.h>
#include <errno.h>
#include <time.h>
#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xmd.h>
#include <X11/Xutil.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/Xinerama.h>

#include "rofi.h"

#ifdef HAVE_I3_IPC_H
#include <errno.h>
#include <linux/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <i3/ipc.h>
#endif

#include <basedir.h>

#include "run-dialog.h"
#include "ssh-dialog.h"
#include "dmenu-dialog.h"
#include "script-dialog.h"

#include "xrmoptions.h"

#define LINE_MARGIN            3

#ifdef HAVE_I3_IPC_H
#define I3_SOCKET_PATH_PROP    "I3_SOCKET_PATH"
// This setting is no longer user configurable, but partial to this file:
int  config_i3_mode = 0;
// Path to HAVE_I3_IPC_H socket.
char *i3_socket_path = NULL;
#endif


xdgHandle    xdg_handle;
const char   *cache_dir  = NULL;
unsigned int NumlockMask = 0;
Display      *display    = NULL;


typedef struct _Switcher
{
    char              name[32];
    switcher_callback cb;
    void              *cb_data;
} Switcher;

Switcher *switchers    = NULL;
int      num_switchers = 0;


void window_set_opacity ( Display *display, Window box, unsigned int opacity );


int switcher_get ( const char *name )
{
    for ( int i = 0; i < num_switchers; i++ ) {
        if ( strcmp ( switchers[i].name, name ) == 0 ) {
            return i;
        }
    }
    return -1;
}


/**
 * Not every platform has strlcpy. (Why god why?)
 * So a quick implementation to fix this.
 */
static size_t copy_string ( char *dest, const char *src, size_t len )
{
    size_t size;

    if ( !len ) {
        return 0;
    }
    size = strlen ( src );
    if ( size >= len ) {
        size = len - 1;
    }
    memcpy ( dest, src, size );
    dest[size] = '\0';
    return size;
}

/**
 * Shared 'token_match' function.
 * Matches tokenized.
 */
int token_match ( char **tokens, const char *input,
                  __attribute__( ( unused ) ) int index,
                  __attribute__( ( unused ) ) void *data )
{
    int  match = 1;

    char *lowerc = g_utf8_casefold ( input, -1 );
    char *compk  = g_utf8_collate_key ( lowerc, -1 );
    // Do a tokenized match.
    if ( tokens ) {
        for ( int j = 0; match && tokens[j]; j++ ) {
            match = ( strstr ( compk, tokens[j] ) != NULL );
        }
    }
    g_free ( lowerc );
    g_free ( compk );
    return match;
}


static char **tokenize ( const char *input )
{
    if ( input == NULL ) {
        return NULL;
    }

    char *saveptr = NULL, *token;
    char **retv   = NULL;
    // First entry is always full (modified) stringtext.
    int  num_tokens = 0;

    // Copy the string, 'strtok_r' modifies it.
    char *str = g_strdup ( input );

    // Iterate over tokens.
    // strtok should still be valid for utf8.
    for ( token = strtok_r ( str, " ", &saveptr );
          token != NULL;
          token = strtok_r ( NULL, " ", &saveptr ) ) {
        // Get case insensitive version of the string.
        char *tmp = g_utf8_casefold ( token, -1 );

        retv                 = g_realloc ( retv, sizeof ( char* ) * ( num_tokens + 2 ) );
        retv[num_tokens + 1] = NULL;
        // Create compare key from the case insensitive version.
        retv[num_tokens] = g_utf8_collate_key ( tmp, -1 );
        num_tokens++;
        g_free ( tmp );
    }
    // Free str.
    g_free ( str );
    return retv;
}

static inline void tokenize_free ( char **ip )
{
    if ( ip == NULL ) {
        return;
    }

    // Free with g_free.
    for ( int i = 0; ip[i] != NULL; i++ ) {
        g_free ( ip[i] );
    }
    g_free ( ip );
}

#ifdef HAVE_I3_IPC_H
// Focus window on HAVE_I3_IPC_H window manager.
static void focus_window_i3 ( const char *socket_path, int id )
{
    i3_ipc_header_t    head;
    char               command[128];
    int                s, t, len;
    struct sockaddr_un remote;

    if ( strlen ( socket_path ) > UNIX_PATH_MAX ) {
        fprintf ( stderr, "Socket path is to long. %zd > %d\n", strlen ( socket_path ), UNIX_PATH_MAX );
        return;
    }

    if ( ( s = socket ( AF_UNIX, SOCK_STREAM, 0 ) ) == -1 ) {
        fprintf ( stderr, "Failed to open connection to I3: %s\n", strerror ( errno ) );
        return;
    }

    remote.sun_family = AF_UNIX;
    strcpy ( remote.sun_path, socket_path );
    len = strlen ( remote.sun_path ) + sizeof ( remote.sun_family );

    if ( connect ( s, ( struct sockaddr * ) &remote, len ) == -1 ) {
        fprintf ( stderr, "Failed to connect to I3 (%s): %s\n", socket_path, strerror ( errno ) );
        close ( s );
        return;
    }


    // Formulate command
    snprintf ( command, 128, "[id=\"%d\"] focus", id );
    // Prepare header.
    memcpy ( head.magic, I3_IPC_MAGIC, 6 );
    head.size = strlen ( command );
    head.type = I3_IPC_MESSAGE_TYPE_COMMAND;
    // Send header.
    send ( s, &head, sizeof ( head ), 0 );
    // Send message
    send ( s, command, strlen ( command ), 0 );
    // Receive header.
    t = recv ( s, &head, sizeof ( head ), 0 );

    if ( t == sizeof ( head ) ) {
        recv ( s, command, head.size, 0 );
    }

    close ( s );
}
#endif

void catch_exit ( __attribute__( ( unused ) ) int sig )
{
    while ( 0 < waitpid ( -1, NULL, WNOHANG ) ) {
        ;
    }
}


// cli arg handling
static int find_arg ( const int argc, char * const argv[], const char * const key )
{
    int i;

    for ( i = 0; i < argc && strcasecmp ( argv[i], key ); i++ ) {
        ;
    }

    return i < argc ? i : -1;
}
static int find_arg_str ( int argc, char *argv[], char *key, char** val )
{
    int i = find_arg ( argc, argv, key );

    if ( val != NULL && i > 0 && i < argc - 1 ) {
        *val = argv[i + 1];
        return TRUE;
    }
    return FALSE;
}

static int find_arg_int ( int argc, char *argv[], char *key, unsigned int *val )
{
    int i = find_arg ( argc, argv, key );

    if ( val != NULL && i > 0 && i < ( argc - 1 ) ) {
        *val = strtol ( argv[i + 1], NULL, 10 );
        return TRUE;
    }
    return FALSE;
}


static int ( *xerror )( Display *, XErrorEvent * );

#define ATOM_ENUM( x )    x
#define ATOM_CHAR( x )    # x

#define EWMH_ATOMS( X )               \
    X ( _NET_CLIENT_LIST_STACKING ),  \
    X ( _NET_NUMBER_OF_DESKTOPS ),    \
    X ( _NET_CURRENT_DESKTOP ),       \
    X ( _NET_ACTIVE_WINDOW ),         \
    X ( _NET_WM_NAME ),               \
    X ( _NET_WM_STATE ),              \
    X ( _NET_WM_STATE_SKIP_TASKBAR ), \
    X ( _NET_WM_STATE_SKIP_PAGER ),   \
    X ( _NET_WM_STATE_ABOVE ),        \
    X ( _NET_WM_DESKTOP ),            \
    X ( I3_SOCKET_PATH ),             \
    X ( CLIPBOARD ),                  \
    X ( UTF8_STRING ),                \
    X ( _NET_WM_WINDOW_OPACITY )

enum { EWMH_ATOMS ( ATOM_ENUM ), NUM_NETATOMS };
const char *netatom_names[] = { EWMH_ATOMS ( ATOM_CHAR ) };
Atom       netatoms[NUM_NETATOMS];

// X error handler
static int display_oops ( Display *d, XErrorEvent *ee )
{
    if ( ee->error_code == BadWindow
         || ( ee->request_code == X_GrabButton && ee->error_code == BadAccess )
         || ( ee->request_code == X_GrabKey && ee->error_code == BadAccess )
         ) {
        return 0;
    }

    fprintf ( stderr, "error: request code=%d, error code=%d\n", ee->request_code, ee->error_code );
    return xerror ( d, ee );
}

// usable space on a monitor
typedef struct
{
    int x, y, w, h;
    int l, r, t, b;
} workarea;


// window lists
typedef struct
{
    Window *array;
    void   **data;
    int    len;
} winlist;

winlist *cache_client = NULL;
winlist *cache_xattr  = NULL;

#define winlist_ascend( l, i, w )     for ( ( i ) = 0; ( i ) < ( l )->len && ( ( ( w ) = ( l )->array[i] ) || 1 ); ( i )++ )
#define winlist_descend( l, i, w )    for ( ( i ) = ( l )->len - 1; ( i ) >= 0 && ( ( ( w ) = ( l )->array[i] ) || 1 ); ( i )-- )

#define WINLIST    32

winlist* winlist_new ()
{
    winlist *l = g_malloc ( sizeof ( winlist ) );
    l->len   = 0;
    l->array = g_malloc_n ( WINLIST + 1, sizeof ( Window ) );
    l->data  = g_malloc_n ( WINLIST + 1, sizeof ( void* ) );
    return l;
}
int winlist_append ( winlist *l, Window w, void *d )
{
    if ( l->len > 0 && !( l->len % WINLIST ) ) {
        l->array = g_realloc ( l->array, sizeof ( Window ) * ( l->len + WINLIST + 1 ) );
        l->data  = g_realloc ( l->data, sizeof ( void* ) * ( l->len + WINLIST + 1 ) );
    }
    // Make clang-check happy.
    // TODO: make clang-check clear this should never be 0.
    if ( l->data == NULL || l->array == NULL ) {
        return 0;
    }

    l->data[l->len]    = d;
    l->array[l->len++] = w;
    return l->len - 1;
}
void winlist_empty ( winlist *l )
{
    while ( l->len > 0 ) {
        g_free ( l->data[--( l->len )] );
    }
}
void winlist_free ( winlist *l )
{
    winlist_empty ( l );
    g_free ( l->array );
    g_free ( l->data );
    g_free ( l );
}
int winlist_find ( winlist *l, Window w )
{
// iterate backwards. theory is: windows most often accessed will be
// nearer the end. testing with kcachegrind seems to support this...
    int    i;
    Window o = 0;

    winlist_descend ( l, i, o ) if ( w == o ) {
        return i;
    }

    return -1;
}

#define CLIENTTITLE    100
#define CLIENTCLASS    50
#define CLIENTNAME     50
#define CLIENTSTATE    10
#define CLIENTROLE     50

// a managable window
typedef struct
{
    Window            window, trans;
    XWindowAttributes xattr;
    char              title[CLIENTTITLE];
    char              class[CLIENTCLASS];
    char              name[CLIENTNAME];
    char              role[CLIENTROLE];
    int               states;
    Atom              state[CLIENTSTATE];
    workarea          monitor;
    int               active;
} client;



// g_malloc a pixel value for an X named color
static unsigned int color_get ( Display *display, const char *const name )
{
    int      screen_id = DefaultScreen ( display );
    XColor   color;
    Colormap map = DefaultColormap ( display, screen_id );
    return XAllocNamedColor ( display, map, name, &color, &color ) ? color.pixel : None;
}

// find mouse pointer location
int pointer_get ( Window root, int *x, int *y )
{
    *x = 0;
    *y = 0;
    Window       rr, cr;
    int          rxr, ryr, wxr, wyr;
    unsigned int mr;

    if ( XQueryPointer ( display, root, &rr, &cr, &rxr, &ryr, &wxr, &wyr, &mr ) ) {
        *x = rxr;
        *y = ryr;
        return 1;
    }

    return 0;
}

static int take_keyboard ( Window w )
{
    int i;

    for ( i = 0; i < 1000; i++ ) {
        if ( XGrabKeyboard ( display, w, True, GrabModeAsync, GrabModeAsync, CurrentTime ) == GrabSuccess ) {
            return 1;
        }

        struct timespec rsl = { 0, 100000L };
        nanosleep ( &rsl, NULL );
    }

    return 0;
}
void release_keyboard ()
{
    XUngrabKeyboard ( display, CurrentTime );
}

// XGetWindowAttributes with caching
XWindowAttributes* window_get_attributes ( Window w )
{
    int idx = winlist_find ( cache_xattr, w );

    if ( idx < 0 ) {
        XWindowAttributes *cattr = g_malloc ( sizeof ( XWindowAttributes ) );

        if ( XGetWindowAttributes ( display, w, cattr ) ) {
            winlist_append ( cache_xattr, w, cattr );
            return cattr;
        }

        g_free ( cattr );
        return NULL;
    }

    return cache_xattr->data[idx];
}

// retrieve a property of any type from a window
int window_get_prop ( Window w, Atom prop, Atom *type, int *items, void *buffer, unsigned int bytes )
{
    Atom _type;

    if ( !type ) {
        type = &_type;
    }

    int _items;

    if ( !items ) {
        items = &_items;
    }

    int           format;
    unsigned long nitems, nbytes;
    unsigned char *ret = NULL;
    memset ( buffer, 0, bytes );

    if ( XGetWindowProperty ( display, w, prop, 0, bytes / 4, False, AnyPropertyType, type,
                              &format, &nitems, &nbytes, &ret ) == Success && ret && *type != None && format ) {
        if ( format == 8 ) {
            memmove ( buffer, ret, MIN ( bytes, nitems ) );
        }

        if ( format == 16 ) {
            memmove ( buffer, ret, MIN ( bytes, nitems * sizeof ( short ) ) );
        }

        if ( format == 32 ) {
            memmove ( buffer, ret, MIN ( bytes, nitems * sizeof ( long ) ) );
        }

        *items = ( int ) nitems;
        XFree ( ret );
        return 1;
    }

    return 0;
}

// retrieve a text property from a window
// technically we could use window_get_prop(), but this is better for character set support
char* window_get_text_prop ( Window w, Atom atom )
{
    XTextProperty prop;
    char          *res   = NULL;
    char          **list = NULL;
    int           count;

    if ( XGetTextProperty ( display, w, &prop, atom ) && prop.value && prop.nitems ) {
        if ( prop.encoding == XA_STRING ) {
            res = g_malloc ( strlen ( ( char * ) prop.value ) + 1 );
            // make clang-check happy.
            if ( res ) {
                strcpy ( res, ( char * ) prop.value );
            }
        }
        else if ( Xutf8TextPropertyToTextList ( display, &prop, &list, &count ) >= Success && count > 0 && *list ) {
            res = g_malloc ( strlen ( *list ) + 1 );
            // make clang-check happy.
            if ( res ) {
                strcpy ( res, *list );
            }
            XFreeStringList ( list );
        }
    }

    if ( prop.value ) {
        XFree ( prop.value );
    }

    return res;
}

int window_get_atom_prop ( Window w, Atom atom, Atom *list, int count )
{
    Atom type;
    int  items;
    return window_get_prop ( w, atom, &type, &items, list, count * sizeof ( Atom ) ) && type == XA_ATOM ? items : 0;
}

void window_set_atom_prop ( Window w, Atom prop, Atom *atoms, int count )
{
    XChangeProperty ( display, w, prop, XA_ATOM, 32, PropModeReplace, ( unsigned char * ) atoms, count );
}

int window_get_cardinal_prop ( Window w, Atom atom, unsigned long *list, int count )
{
    Atom type; int items;
    return window_get_prop ( w, atom, &type, &items, list, count * sizeof ( unsigned long ) ) && type == XA_CARDINAL ? items : 0;
}

// a ClientMessage
int window_send_message ( Window target, Window subject, Atom atom, unsigned long protocol, unsigned long mask, Time time )
{
    XEvent e;
    memset ( &e, 0, sizeof ( XEvent ) );
    e.xclient.type         = ClientMessage;
    e.xclient.message_type = atom;
    e.xclient.window       = subject;
    e.xclient.data.l[0]    = protocol;
    e.xclient.data.l[1]    = time;
    e.xclient.send_event   = True;
    e.xclient.format       = 32;
    int r = XSendEvent ( display, target, False, mask, &e ) ? 1 : 0;
    XFlush ( display );
    return r;
}

// find the dimensions of the monitor displaying point x,y
void monitor_dimensions ( Screen *screen, int x, int y, workarea *mon )
{
    memset ( mon, 0, sizeof ( workarea ) );
    mon->w = WidthOfScreen ( screen );
    mon->h = HeightOfScreen ( screen );

    // locate the current monitor
    if ( XineramaIsActive ( display ) ) {
        int                monitors;
        XineramaScreenInfo *info = XineramaQueryScreens ( display, &monitors );

        if ( info ) {
            for ( int i = 0; i < monitors; i++ ) {
                if ( INTERSECT ( x, y, 1, 1, info[i].x_org, info[i].y_org, info[i].width, info[i].height ) ) {
                    mon->x = info[i].x_org;
                    mon->y = info[i].y_org;
                    mon->w = info[i].width;
                    mon->h = info[i].height;
                    break;
                }
            }
        }

        XFree ( info );
    }
}

// determine which monitor holds the active window, or failing that the mouse pointer
void monitor_active ( workarea *mon )
{
    Screen *screen = DefaultScreenOfDisplay ( display );
    Window root    = RootWindow ( display, XScreenNumberOfScreen ( screen ) );
    int    x, y;

    if ( pointer_get ( root, &x, &y ) ) {
        monitor_dimensions ( screen, x, y, mon );
        return;
    }

    monitor_dimensions ( screen, 0, 0, mon );
}

// _NET_WM_STATE_*
int client_has_state ( client *c, Atom state )
{
    int i;

    for ( i = 0; i < c->states; i++ ) {
        if ( c->state[i] == state ) {
            return 1;
        }
    }

    return 0;
}

// collect info on any window
// doesn't have to be a window we'll end up managing
client* window_client ( Window win )
{
    if ( win == None ) {
        return NULL;
    }

    int idx = winlist_find ( cache_client, win );

    if ( idx >= 0 ) {
        return cache_client->data[idx];
    }

    // if this fails, we're up that creek
    XWindowAttributes *attr = window_get_attributes ( win );

    if ( !attr ) {
        return NULL;
    }

    client *c = g_malloc0 ( sizeof ( client ) );
    c->window = win;

    // copy xattr so we don't have to care when stuff is freed
    memmove ( &c->xattr, attr, sizeof ( XWindowAttributes ) );
    XGetTransientForHint ( display, win, &c->trans );

    c->states = window_get_atom_prop ( win, netatoms[_NET_WM_STATE], c->state, CLIENTSTATE );

    char *name;

    if ( ( name = window_get_text_prop ( c->window, netatoms[_NET_WM_NAME] ) ) && name ) {
        snprintf ( c->title, CLIENTTITLE, "%s", name );
        g_free ( name );
    }
    else if ( XFetchName ( display, c->window, &name ) ) {
        snprintf ( c->title, CLIENTTITLE, "%s", name );
        XFree ( name );
    }

    name = window_get_text_prop ( c->window, XInternAtom ( display, "WM_WINDOW_ROLE", False ) );

    if ( name != NULL ) {
        snprintf ( c->role, CLIENTROLE, "%s", name );
        XFree ( name );
    }

    XClassHint chint;

    if ( XGetClassHint ( display, c->window, &chint ) ) {
        snprintf ( c->class, CLIENTCLASS, "%s", chint.res_class );
        snprintf ( c->name, CLIENTNAME, "%s", chint.res_name );
        XFree ( chint.res_class );
        XFree ( chint.res_name );
    }

    monitor_dimensions ( c->xattr.screen, c->xattr.x, c->xattr.y, &c->monitor );
    winlist_append ( cache_client, c->window, c );
    return c;
}

unsigned int windows_modmask;
KeySym       windows_keysym;
unsigned int rundialog_modmask;
KeySym       rundialog_keysym;
unsigned int sshdialog_modmask;
KeySym       sshdialog_keysym;

Window       main_window = None;
GC           gc          = NULL;

#include "textbox.h"

void menu_hide_arrow_text ( int filtered_lines, int selected, int max_elements,
                            textbox *arrowbox_top, textbox *arrowbox_bottom )
{
    if ( arrowbox_top == NULL || arrowbox_bottom == NULL ) {
        return;
    }
    int page   = ( filtered_lines > 0 ) ? selected / max_elements : 0;
    int npages = ( filtered_lines > 0 ) ? ( ( filtered_lines + max_elements - 1 ) / max_elements ) : 1;
    if ( !( page != 0 && npages > 1 ) ) {
        textbox_hide ( arrowbox_top );
    }
    if ( !( ( npages - 1 ) != page && npages > 1 ) ) {
        textbox_hide ( arrowbox_bottom );
    }
}

void menu_set_arrow_text ( int filtered_lines, int selected, int max_elements,
                           textbox *arrowbox_top, textbox *arrowbox_bottom )
{
    if ( arrowbox_top == NULL || arrowbox_bottom == NULL ) {
        return;
    }
    if ( filtered_lines == 0 || max_elements == 0 ) {
        return;
    }
    int page   = ( filtered_lines > 0 ) ? selected / max_elements : 0;
    int npages = ( filtered_lines > 0 ) ? ( ( filtered_lines + max_elements - 1 ) / max_elements ) : 1;
    int entry  = selected % max_elements;
    if ( page != 0 && npages > 1 ) {
        textbox_show ( arrowbox_top );
        textbox_font ( arrowbox_top, ( entry != 0 ) ? NORMAL : HIGHLIGHT );
        textbox_draw ( arrowbox_top  );
    }
    if ( ( npages - 1 ) != page && npages > 1 ) {
        textbox_show ( arrowbox_bottom );
        textbox_font ( arrowbox_bottom, ( entry != ( max_elements - 1 ) ) ? NORMAL : HIGHLIGHT );
        textbox_draw ( arrowbox_bottom  );
    }
}

void menu_draw ( textbox **boxes,
                 int max_elements,
                 int num_lines,
                 int *last_offset,
                 int selected,
                 char **filtered )
{
    int i, offset = 0;

    // selected row is always visible.
    // If selected is visible do not scroll.
    if ( ( ( selected - ( *last_offset ) ) < ( max_elements ) )
         && ( ( selected - ( *last_offset ) ) >= 0 ) ) {
        offset = *last_offset;
    }
    else{
        // Do paginating
        int page = ( max_elements > 0 ) ? ( selected / max_elements ) : 0;
        offset       = page * max_elements;
        *last_offset = offset;
    }

    for ( i = 0; i < max_elements; i++ ) {
        if ( ( i + offset ) >= num_lines || filtered[i + offset] == NULL ) {
            textbox_font ( boxes[i], NORMAL );
            textbox_text ( boxes[i], "" );
        }
        else{
            char            *text = filtered[i + offset];
            TextBoxFontType tbft  = ( i + offset ) == selected ? HIGHLIGHT : NORMAL;
            textbox_font ( boxes[i], tbft );
            textbox_text ( boxes[i], text );
        }

        textbox_draw ( boxes[i] );
    }
}

int window_match ( char **tokens, __attribute__( ( unused ) ) const char *input, int index, void *data )
{
    int     match = 1;
    winlist *ids  = ( winlist * ) data;
    client  *c    = window_client ( ids->array[index] );


    if ( tokens ) {
        for ( int j = 0; match && tokens[j]; j++ ) {
            int  test = 0;

            char *sml = g_utf8_casefold ( c->title, -1 );
            char *key = g_utf8_collate_key ( sml, -1 );
            if ( !test && c->title[0] != '\0' ) {
                test = ( strstr ( key, tokens[j] ) != NULL );
            }
            g_free ( sml ); g_free ( key );

            sml = g_utf8_casefold ( c->class, -1 );
            key = g_utf8_collate_key ( sml, -1 );
            if ( !test && c->class[0] != '\0' ) {
                test = ( strstr ( key, tokens[j] ) != NULL );
            }
            g_free ( sml ); g_free ( key );

            sml = g_utf8_casefold ( c->role, -1 );
            key = g_utf8_collate_key ( sml, -1 );
            if ( !test && c->role[0] != '\0' ) {
                test = ( strstr ( key, tokens[j] ) != NULL );
            }
            g_free ( sml ); g_free ( key );

            sml = g_utf8_casefold ( c->name, -1 );
            key = g_utf8_collate_key ( sml, -1 );
            if ( !test && c->name[0] != '\0' ) {
                test = ( strstr ( key, tokens[j] ) != NULL );
            }
            g_free ( sml ); g_free ( key );

            if ( test == 0 ) {
                match = 0;
            }
        }
    }

    return match;
}

static int lev_sort ( const void *p1, const void *p2, void *arg )
{
    const int *a         = p1;
    const int *b         = p2;
    int       *distances = arg;

    return distances[*a] - distances[*b];
}

static int levenshtein ( const char *s, const char *t )
{
    int ls = strlen ( s ), lt = strlen ( t );
    int d[ls + 1][lt + 1];

    for ( int i = 0; i <= ls; i++ ) {
        for ( int j = 0; j <= lt; j++ ) {
            d[i][j] = -1;
        }
    }

    int dist ( int i, int j )
    {
        if ( d[i][j] >= 0 ) {
            return d[i][j];
        }

        int x;
        if ( i == ls ) {
            x = lt - j;
        }
        else if ( j == lt ) {
            x = ls - i;
        }
        else if ( s[i] == t[j] ) {
            x = dist ( i + 1, j + 1 );
        }
        else {
            x = dist ( i + 1, j + 1 );

            int y;
            if ( ( y = dist ( i, j + 1 ) ) < x ) {
                x = y;
            }
            if ( ( y = dist ( i + 1, j ) ) < x ) {
                x = y;
            }
            x++;
        }
        return d[i][j] = x;
    }
    return dist ( 0, 0 );
}

void window_set_opacity ( Display *display, Window box, unsigned int opacity )
{
    // Hack to set window opacity.
    unsigned int opacity_set = ( unsigned int ) ( ( opacity / 100.0 ) * UINT32_MAX );
    XChangeProperty ( display, box, netatoms[_NET_WM_WINDOW_OPACITY],
                      XA_CARDINAL, 32, PropModeReplace,
                      ( unsigned char * ) &opacity_set, 1L );
}

Window create_window ( Display *display )
{
    Screen *screen = DefaultScreenOfDisplay ( display );
    Window root    = RootWindow ( display, XScreenNumberOfScreen ( screen ) );
    Window box     = XCreateSimpleWindow ( display, root, 0, 0, 200, 100,
                                           config.menu_bw,
                                           color_get ( display, config.menu_bc ),
                                           color_get ( display, config.menu_bg ) );
    XSelectInput ( display, box, ExposureMask );

    gc = XCreateGC ( display, box, 0, 0 );
    XSetLineAttributes ( display, gc, 2, LineOnOffDash, CapButt, JoinMiter );
    XSetForeground ( display, gc, color_get ( display, config.menu_bc ) );
    // make it an unmanaged window
    window_set_atom_prop ( box, netatoms[_NET_WM_STATE], &netatoms[_NET_WM_STATE_ABOVE], 1 );
    XSetWindowAttributes sattr;
    sattr.override_redirect = True;
    XChangeWindowAttributes ( display, box, CWOverrideRedirect, &sattr );

    // Set the WM_NAME
    XStoreName ( display, box, "rofi" );

    window_set_opacity ( display, box, config.window_opacity );
    return box;
}

/**
 * @param x [out] the calculated x position.
 * @param y [out] the calculated y position.
 * @param mon     the workarea.
 * @param h       the required height of the window.
 * @param w       the required width of the window.
 */
static void calculate_window_position ( const workarea *mon, int *x, int *y, int w, int h )
{
    // Default location is center.
    *y = mon->y + ( mon->h - h - config.menu_bw * 2 ) / 2;
    *x = mon->x + ( mon->w - w - config.menu_bw * 2 ) / 2;
    // Determine window location
    switch ( config.location )
    {
    case WL_NORTH_WEST:
        *x = mon->x;

    case WL_NORTH:
        *y = mon->y;
        break;

    case WL_NORTH_EAST:
        *y = mon->y;

    case WL_EAST:
        *x = mon->x + mon->w - w - config.menu_bw * 2;
        break;

    case WL_EAST_SOUTH:
        *x = mon->x + mon->w - w - config.menu_bw * 2;

    case WL_SOUTH:
        *y = mon->y + mon->h - h - config.menu_bw * 2;
        break;

    case WL_SOUTH_WEST:
        *y = mon->y + mon->h - h - config.menu_bw * 2;

    case WL_WEST:
        *x = mon->x;
        break;

    case WL_CENTER:
    default:
        break;
    }
    // Apply offset.
    *x += config.x_offset;
    *y += config.y_offset;
}

MenuReturn menu ( char **lines, unsigned int num_lines, char **input, char *prompt, Time *time,
                  int *shift, menu_match_cb mmc, void *mmc_data, int *selected_line, int sorting )
{
    int          retv = MENU_CANCEL;
    unsigned int i, j;
    unsigned int columns = config.menu_columns;
    workarea     mon;
    unsigned int max_elements = MIN ( config.menu_lines * columns, num_lines );

    // Calculate the number or rows. We do this by getting the num_lines rounded up to X columns
    // (num elements is better name) then dividing by columns.
    unsigned int max_rows = MIN ( config.menu_lines,
                                  (unsigned int) (
                                      ( num_lines + ( columns - num_lines % columns ) % columns ) /
                                      ( columns )
                                      ) );

    if ( config.fixed_num_lines == TRUE ) {
        max_elements = config.menu_lines * columns;
        max_rows     = config.menu_lines;
        // If it would fit in one column, only use one column.
        if ( num_lines < max_elements ) {
            columns      = ( num_lines + ( max_rows - num_lines % max_rows ) % max_rows ) / max_rows;
            max_elements = config.menu_lines * columns;
        }
        // Sanitize.
        if ( columns == 0 ) {
            columns = 1;
        }
    }
    // More hacks.
    if ( config.hmode == TRUE ) {
        max_rows = 1;
    }

    // Get active monitor size.
    monitor_active ( &mon );

    // Calculate as float to stop silly, big rounding down errors.
    int w = config.menu_width < 101 ? ( mon.w / 100.0f ) * ( float ) config.menu_width : config.menu_width;
    // Compensate for border width.
    w -= config.menu_bw * 2;

    int element_width = w - ( 2 * ( config.padding ) );
    // Divide by the # columns
    element_width = ( element_width - ( columns - 1 ) * LINE_MARGIN ) / columns;
    if ( config.hmode == TRUE ) {
        element_width = ( w - ( 2 * ( config.padding ) ) - max_elements * LINE_MARGIN ) / ( max_elements + 1 );
    }

    // main window isn't explicitly destroyed in case we switch modes. Reusing it prevents flicker
    XWindowAttributes attr;
    if ( main_window == None || XGetWindowAttributes ( display, main_window, &attr ) == 0 ) {
        main_window = create_window ( display );
    }

    // search text input

    textbox *prompt_tb = textbox_create ( main_window, TB_AUTOHEIGHT | TB_AUTOWIDTH,
                                          ( config.padding ), ( config.padding ),
                                          0, 0, NORMAL, prompt );

    textbox *text = textbox_create ( main_window, TB_AUTOHEIGHT | TB_EDITABLE,
                                     ( config.padding ) + textbox_get_width ( prompt_tb ),
                                     ( config.padding ),
                                     ( ( config.hmode == TRUE ) ?
                                       element_width : ( w - ( 2 * ( config.padding ) ) ) )
                                     - textbox_get_width ( prompt_tb ), 1,
                                     NORMAL,
                                     ( input != NULL ) ? *input : "" );

    int line_height = textbox_get_height ( text ); //text->font->ascent + text->font->descent;

    textbox_show ( text );
    textbox_show ( prompt_tb );

    // filtered list display
    textbox **boxes = g_malloc0_n ( max_elements, sizeof ( textbox* ) );

    for ( i = 0; i < max_elements; i++ ) {
        int line = ( i ) % max_rows + ( ( config.hmode == FALSE ) ? 1 : 0 );
        int col  = ( i ) / max_rows + ( ( config.hmode == FALSE ) ? 0 : 1 );

        int ex = ( config.padding ) + col * ( element_width + LINE_MARGIN );
        int ey = line * line_height + config.padding + ( ( config.hmode == TRUE ) ? 0 : LINE_MARGIN );
        boxes[i] = textbox_create ( main_window, 0, ex, ey, element_width, line_height, NORMAL, "" );
        textbox_show ( boxes[i] );
    }
    // Arrows
    textbox *arrowbox_top = NULL, *arrowbox_bottom = NULL;
    arrowbox_top = textbox_create ( main_window, TB_AUTOHEIGHT | TB_AUTOWIDTH,
                                    ( config.padding ),
                                    ( config.padding ),
                                    0, 0,
                                    NORMAL,
                                    ( config.hmode == FALSE ) ? "↑" : "←" );
    arrowbox_bottom = textbox_create ( main_window, TB_AUTOHEIGHT | TB_AUTOWIDTH,
                                       ( config.padding ),
                                       ( config.padding ),
                                       0, 0,
                                       NORMAL,
                                       ( config.hmode == FALSE ) ? "↓" : "→" );

    if ( config.hmode == FALSE ) {
        textbox_move ( arrowbox_top,
                       w - config.padding - arrowbox_top->w,
                       config.padding + line_height + LINE_MARGIN );
        textbox_move ( arrowbox_bottom,
                       w - config.padding - arrowbox_bottom->w,
                       config.padding + max_rows * line_height + LINE_MARGIN );
    }
    else {
        textbox_move ( arrowbox_bottom,
                       w - config.padding - arrowbox_top->w,
                       config.padding );
        textbox_move ( arrowbox_top,
                       w - config.padding - arrowbox_bottom->w - arrowbox_top->w,
                       config.padding );
    }

    // filtered list
    char **filtered = g_malloc0_n ( num_lines, sizeof ( char* ) );
    int  *line_map  = g_malloc0_n ( num_lines, sizeof ( int ) );
    int  *distance  = NULL;
    if ( sorting ) {
        distance = g_malloc0_n ( num_lines, sizeof ( int ) );
    }
    unsigned int filtered_lines = 0;
    // We want to filter on the first run.
    int          refilter = TRUE;
    int          update   = FALSE;

    // resize window vertically to suit
    // Subtract the margin of the last row.
    int h = line_height * ( max_rows + 1 ) + ( config.padding ) * 2 + LINE_MARGIN;
    if ( config.hmode == TRUE ) {
        h = line_height + ( config.padding ) * 2;
    }

    // Move the window to the correct x,y position.
    int x, y;
    calculate_window_position ( &mon, &x, &y, w, h );
    XMoveResizeWindow ( display, main_window, x, y, w, h );

    XMapRaised ( display, main_window );

    // if grabbing keyboard failed, fall through
    if ( take_keyboard ( main_window ) ) {
        KeySym       prev_key    = 0;
        unsigned int selected    = 0;
        int          last_offset = 0;
        int          init        = 0;
        if ( selected_line != NULL ) {
            if ( *selected_line >= 0 && *selected_line <= num_lines ) {
                selected = *selected_line;
            }
        }

        for (;; ) {
            // If something changed, refilter the list. (paste or text entered)
            if ( refilter ) {
                if ( strlen ( text->text ) > 0 ) {
                    char **tokens = tokenize ( text->text );

                    // input changed
                    for ( i = 0, j = 0; i < num_lines; i++ ) {
                        int match = mmc ( tokens, lines[i], i, mmc_data );

                        // If each token was matched, add it to list.
                        if ( match ) {
                            line_map[j] = i;
                            if ( sorting ) {
                                distance[i] = levenshtein ( text->text, lines[i] );
                            }
                            // Try to look-up the selected line and highlight that.
                            // This is needed 'hack' to fix the dmenu 'next row' modi.
                            if ( init == 0 && selected_line != NULL && ( *selected_line ) == i ) {
                                selected = j;
                                init     = 1;
                            }
                            j++;
                        }
                    }
                    if ( sorting ) {
                        qsort_r ( line_map, j, sizeof ( int ), lev_sort, distance );
                    }
                    // Update the filtered list.
                    for ( i = 0; i < j; i++ ) {
                        filtered[i] = lines[line_map[i]];
                    }
                    for ( i = j; i < num_lines; i++ ) {
                        filtered[i] = NULL;
                    }

                    // Cleanup + bookkeeping.
                    filtered_lines = j;
                    tokenize_free ( tokens );
                }
                else{
                    for ( i = 0; i < num_lines; i++ ) {
                        filtered[i] = lines[i];
                        line_map[i] = i;
                    }
                    filtered_lines = num_lines;
                }
                selected = MIN ( selected, j - 1 );

                if ( config.zeltak_mode && filtered_lines == 1 ) {
                    if ( filtered[selected] ) {
                        retv           = MENU_OK;
                        *selected_line = line_map[selected];
                    }
                    else{
                        fprintf ( stderr, "We should never hit this." );
                        abort ();
                    }

                    break;
                }
                refilter = FALSE;
            }
            // Update if requested.
            if ( update ) {
                menu_hide_arrow_text ( filtered_lines, selected,
                                       max_elements, arrowbox_top,
                                       arrowbox_bottom );
                textbox_draw ( text );
                textbox_draw ( prompt_tb );
                menu_draw ( boxes, max_elements, num_lines, &last_offset, selected, filtered );
                menu_set_arrow_text ( filtered_lines, selected,
                                      max_elements, arrowbox_top,
                                      arrowbox_bottom );
                update = FALSE;
            }

            // Wait for event.
            XEvent ev;
            XNextEvent ( display, &ev );

            // Handle event.
            if ( ev.type == Expose ) {
                while ( XCheckTypedEvent ( display, Expose, &ev ) ) {
                    ;
                }

                textbox_draw ( text );
                textbox_draw ( prompt_tb );
                menu_draw ( boxes, max_elements, num_lines, &last_offset, selected, filtered );
                menu_set_arrow_text ( filtered_lines, selected,
                                      max_elements, arrowbox_top,
                                      arrowbox_bottom );
                // Why do we need the specian -1?
                if ( config.hmode == FALSE && max_elements > 0 ) {
                    XDrawLine ( display, main_window, gc, ( config.padding ),
                                line_height + ( config.padding ) + ( LINE_MARGIN - 2 ) / 2,
                                w - ( ( config.padding ) ) - 1,
                                line_height + ( config.padding ) + ( LINE_MARGIN - 2 ) / 2 );
                }
            }
            else if ( ev.type == SelectionNotify ) {
                // TODO move this.
                if ( ev.xselection.property == netatoms[UTF8_STRING] ) {
                    char          *pbuf = NULL;
                    int           di;
                    unsigned long dl, rm;
                    Atom          da;

                    /* we have been given the current selection, now insert it into input */
                    XGetWindowProperty (
                        display,
                        main_window,
                        netatoms[UTF8_STRING],
                        0,
                        256 / 4,   // max length in words.
                        False,     // Do not delete clipboard.
                        netatoms[UTF8_STRING], &da, &di, &dl, &rm, (unsigned char * *) &pbuf );
                    // If There was remaining data left.. lets ignore this.
                    // Only accept it when we get bytes!
                    if ( di == 8 ) {
                        char *index;
                        if ( ( index = strchr ( pbuf, '\n' ) ) != NULL ) {
                            // Calc new length;
                            dl = index - pbuf;
                        }
                        // Create a NULL terminated string. I am not sure how the data is returned.
                        // With or without trailing 0
                        char str[dl + 1];
                        memcpy ( str, pbuf, dl );
                        str[dl] = '\0';

                        // Insert string move cursor.
                        textbox_insert ( text, text->cursor, str );
                        textbox_cursor ( text, text->cursor + dl );
                        // Force a redraw and refiltering of the text.
                        update   = TRUE;
                        refilter = TRUE;
                    }
                    XFree ( pbuf );
                }
            }
            else if ( ev.type == KeyPress ) {
                while ( XCheckTypedEvent ( display, KeyPress, &ev ) ) {
                    ;
                }

                if ( time ) {
                    *time = ev.xkey.time;
                }

                KeySym key = XkbKeycodeToKeysym ( display, ev.xkey.keycode, 0, 0 );

                if ( ( ( ( ev.xkey.state & ControlMask ) == ControlMask ) && key == XK_v ) ) {
                    XConvertSelection ( display, ( ev.xkey.state & ShiftMask ) ?
                                        XA_PRIMARY : netatoms[CLIPBOARD],
                                        netatoms[UTF8_STRING], netatoms[UTF8_STRING], main_window, CurrentTime );
                }
                if ( key == XK_Insert ) {
                    XConvertSelection ( display, ( ev.xkey.state & ShiftMask ) ?
                                        XA_PRIMARY : netatoms[CLIPBOARD],
                                        netatoms[UTF8_STRING], netatoms[UTF8_STRING], main_window, CurrentTime );
                }
                else if ( ( ( ev.xkey.state & ShiftMask ) == ShiftMask ) &&
                          key == XK_slash ) {
                    retv           = MENU_NEXT;
                    *selected_line = 0;
                    break;
                }
                else if ( ( ( ev.xkey.state & ShiftMask ) == ShiftMask ) &&
                          key == XK_Delete ) {
                    if ( filtered[selected] != NULL ) {
                        *selected_line = line_map[selected];
                        retv           = MENU_ENTRY_DELETE;
                        break;
                    }
                }
                else if ( ( ( ev.xkey.state & Mod1Mask ) == Mod1Mask ) &&
                          key >= XK_1 && key <= XK_9 ) {
                    *selected_line = ( key - XK_1 );
                    retv           = MENU_QUICK_SWITCH;
                    break;
                }

                int rc = textbox_keypress ( text, &ev );

                if ( rc < 0 ) {
                    if ( shift != NULL ) {
                        ( *shift ) = ( ( ev.xkey.state & ShiftMask ) == ShiftMask );
                    }
                    if ( filtered[selected] != NULL ) {
                        retv           = MENU_OK;
                        *selected_line = line_map[selected];
                    }
                    else{
                        retv = MENU_CUSTOM_INPUT;
                    }

                    break;
                }
                else if ( rc ) {
                    refilter = TRUE;
                    update   = TRUE;
                }
                else{
                    // unhandled key
                    KeySym key = XkbKeycodeToKeysym ( display, ev.xkey.keycode, 0, 0 );

                    if ( key == XK_Escape
                         // pressing one of the global key bindings closes the switcher. this allows fast closing of the menu if an item is not selected
                         || ( ( windows_modmask == AnyModifier || ev.xkey.state & windows_modmask ) && key == windows_keysym )
                         || ( ( rundialog_modmask == AnyModifier || ev.xkey.state & rundialog_modmask ) && key == rundialog_keysym )
                         || ( ( sshdialog_modmask == AnyModifier || ev.xkey.state & sshdialog_modmask ) && key == sshdialog_keysym )
                         ) {
                        retv = MENU_CANCEL;
                        break;
                    }
                    else{
                        // Up, Ctrl-p or Shift-Tab
                        if ( key == XK_Up || ( key == XK_Tab && ev.xkey.state & ShiftMask ) ||
                             ( key == XK_p && ev.xkey.state & ControlMask )  ) {
                            if ( selected == 0 ) {
                                selected = filtered_lines;
                            }

                            if ( selected > 0 ) {
                                selected--;
                            }
                            update = TRUE;
                        }
                        // Down, Ctrl-n
                        else if ( key == XK_Down ||
                                  ( key == XK_n && ev.xkey.state & ControlMask ) ) {
                            selected = selected < filtered_lines - 1 ? MIN ( filtered_lines - 1, selected + 1 ) : 0;
                            update   = TRUE;
                        }
                        else if ( key == XK_Page_Up && ev.xkey.state & ControlMask ) {
                            if ( selected < max_rows ) {
                                selected = 0;
                            }
                            else{
                                selected -= max_rows;
                            }
                            update = TRUE;
                        }
                        else if (  key == XK_Page_Down && ev.xkey.state & ControlMask ) {
                            selected += max_rows;
                            if ( selected >= filtered_lines ) {
                                selected = filtered_lines - 1;
                            }
                            update = TRUE;
                        }
                        else if ( key == XK_Page_Up ) {
                            if ( selected < max_elements ) {
                                selected = 0;
                            }
                            else{
                                selected -= ( max_elements );
                            }
                            update = TRUE;
                        }
                        else if ( key == XK_Page_Down ) {
                            selected += ( max_elements );

                            if ( selected >= filtered_lines ) {
                                selected = filtered_lines - 1;
                            }
                            update = TRUE;
                        }
                        else if ( key == XK_Home || key == XK_KP_Home ) {
                            selected = 0;
                            update   = TRUE;
                        }
                        else if ( key == XK_End || key == XK_KP_End ) {
                            selected = filtered_lines - 1;
                            update   = TRUE;
                        }
                        else if ( key == XK_Tab ) {
                            if ( filtered_lines == 1 ) {
                                if ( filtered[selected] ) {
                                    retv           = MENU_OK;
                                    *selected_line = line_map[selected];
                                }
                                else{
                                    fprintf ( stderr, "We should never hit this." );
                                    abort ();
                                }

                                break;
                            }

                            // Double tab!
                            if ( filtered_lines == 0 && key == prev_key ) {
                                retv           = MENU_NEXT;
                                *selected_line = 0;
                                break;
                            }
                            else{
                                selected = selected < filtered_lines - 1 ? MIN ( filtered_lines - 1, selected + 1 ) : 0;
                                update   = TRUE;
                            }
                        }
                    }
                }
                prev_key = key;
            }
        }

        release_keyboard ();
    }

    g_free ( *input );

    *input = g_strdup ( text->text );

    textbox_free ( text );
    textbox_free ( prompt_tb );
    textbox_free ( arrowbox_bottom );
    textbox_free ( arrowbox_top );

    for ( i = 0; i < max_elements; i++ ) {
        textbox_free ( boxes[i] );
    }

    g_free ( boxes );

    g_free ( filtered );
    g_free ( line_map );
    g_free ( distance );

    return retv;
}

SwitcherMode run_switcher_window ( char **input, void *data )
{
    Screen       *screen = DefaultScreenOfDisplay ( display );
    Window       root    = RootWindow ( display, XScreenNumberOfScreen ( screen ) );
    SwitcherMode retv    = MODE_EXIT;
    // find window list
    Atom         type;
    int          nwins;
    Window       wins[100];
    int          count       = 0;
    Window       curr_win_id = 0;

    // Get the active window so we can highlight this.
    if ( !( window_get_prop ( root, netatoms[_NET_ACTIVE_WINDOW], &type,
                              &count, &curr_win_id, sizeof ( Window ) )
            && type == XA_WINDOW && count > 0 ) ) {
        curr_win_id = 0;
    }

    if ( window_get_prop ( root, netatoms[_NET_CLIENT_LIST_STACKING],
                           &type, &nwins, wins, 100 * sizeof ( Window ) )
         && type == XA_WINDOW ) {
        char          pattern[50];
        int           i;
        unsigned int  classfield = 0;
        unsigned long desktops   = 0;
        // windows we actually display. may be slightly different to _NET_CLIENT_LIST_STACKING
        // if we happen to have a window destroyed while we're working...
        winlist *ids = winlist_new ();



        // calc widths of fields
        for ( i = nwins - 1; i > -1; i-- ) {
            client *c;

            if ( ( c = window_client ( wins[i] ) )
                 && !c->xattr.override_redirect
                 && !client_has_state ( c, netatoms[_NET_WM_STATE_SKIP_PAGER] )
                 && !client_has_state ( c, netatoms[_NET_WM_STATE_SKIP_TASKBAR] ) ) {
                classfield = MAX ( classfield, strlen ( c->class ) );

#ifdef HAVE_I3_IPC_H

                // In i3 mode, skip the i3bar completely.
                if ( config_i3_mode && strstr ( c->class, "i3bar" ) != NULL ) {
                    continue;
                }

#endif
                if ( c->window == curr_win_id ) {
                    c->active = TRUE;
                }
                winlist_append ( ids, c->window, NULL );
            }
        }

        // Create pattern for printing the line.
        if ( !window_get_cardinal_prop ( root, netatoms[_NET_NUMBER_OF_DESKTOPS], &desktops, 1 ) ) {
            desktops = 1;
        }
#ifdef HAVE_I3_IPC_H
        if ( config_i3_mode ) {
            sprintf ( pattern, "%%-%ds   %%s", MAX ( 5, classfield ) );
        }
        else{
#endif
        sprintf ( pattern, "%%-%ds  %%-%ds   %%s", desktops < 10 ? 1 : 2, MAX ( 5, classfield ) );
#ifdef HAVE_I3_IPC_H
    }
#endif
        char **list        = g_malloc0_n ( ( ids->len + 1 ), sizeof ( char* ) );
        unsigned int lines = 0;

        // build the actual list
        Window w = 0;
        winlist_ascend ( ids, i, w )
        {
            client *c;

            if ( ( c = window_client ( w ) ) ) {
                // final line format
                unsigned long wmdesktop;
                char          desktop[5];
                desktop[0] = 0;
                char          *line = g_malloc ( strlen ( c->title ) + strlen ( c->class ) + classfield + 50 );
#ifdef HAVE_I3_IPC_H
                if ( !config_i3_mode ) {
#endif
                // find client's desktop. this is zero-based, so we adjust by since most
                // normal people don't think like this :-)
                if ( !window_get_cardinal_prop ( c->window, netatoms[_NET_WM_DESKTOP], &wmdesktop, 1 ) ) {
                    wmdesktop = 0xFFFFFFFF;
                }

                if ( wmdesktop < 0xFFFFFFFF ) {
                    sprintf ( desktop, "%d", (int) wmdesktop + 1 );
                }

                sprintf ( line, pattern, desktop, c->class, c->title );
#ifdef HAVE_I3_IPC_H
            }
            else{
                sprintf ( line, pattern, c->class, c->title );
            }
#endif

                list[lines++] = line;
            }
        }
        Time time;
        int selected_line = 0;
        MenuReturn mretv  = menu ( list, lines, input, "window:", &time, NULL,
                                   window_match, ids, &selected_line, config.levenshtein_sort );

        if ( mretv == MENU_NEXT ) {
            retv = NEXT_DIALOG;
        }
        else if ( mretv == MENU_QUICK_SWITCH ) {
            retv = selected_line;
        }
        else if ( mretv == MENU_OK && list[selected_line] ) {
#ifdef HAVE_I3_IPC_H

            if ( config_i3_mode ) {
                // Hack for i3.
                focus_window_i3 ( i3_socket_path, ids->array[selected_line] );
            }
            else
#endif
            {
                // Change to the desktop of the selected window/client.
                // TODO: get rid of strtol
                window_send_message ( root, root, netatoms[_NET_CURRENT_DESKTOP], strtol ( list[selected_line], NULL, 10 ) - 1,
                                      SubstructureNotifyMask | SubstructureRedirectMask, time );
                XSync ( display, False );

                window_send_message ( root, ids->array[selected_line], netatoms[_NET_ACTIVE_WINDOW], 2, // 2 = pager
                                      SubstructureNotifyMask | SubstructureRedirectMask, time );
            }
        }


        for ( i = 0; i < lines; i++ ) {
            g_free ( list[i] );
        }

        g_free ( list );
        winlist_free ( ids );
    }

    return retv;
}

static int run_dmenu ()
{
    int ret_state = TRUE;
    textbox_setup (
        config.menu_bg, config.menu_fg,
        config.menu_hlbg,
        config.menu_hlfg );
    char *input = NULL;

    // Dmenu modi has a return state.
    ret_state = dmenu_switcher_dialog ( &input );

    g_free ( input );

    // Cleanup font setup.
    textbox_cleanup ();
    return ret_state;
}

static void run_switcher ( int do_fork, SwitcherMode mode )
{
    // we fork because it's technically possible to have multiple window
    // lists up at once on a zaphod multihead X setup.
    // this also happens to isolate the Xft font stuff in a child process
    // that gets cleaned up every time. that library shows some valgrind
    // strangeness...
    if ( do_fork == TRUE ) {
        if ( fork () ) {
            return;
        }

        display = XOpenDisplay ( 0 );
        XSync ( display, True );
    }
    // Because of the above fork, we want to do this here.
    // Make sure this is isolated to its own thread.
    textbox_setup (
        config.menu_bg, config.menu_fg,
        config.menu_hlbg,
        config.menu_hlfg );
    char *input = NULL;
    // Otherwise check if requested mode is enabled.
    if ( switchers[mode].cb != NULL ) {
        do {
            SwitcherMode retv = MODE_EXIT;

            retv = switchers[mode].cb ( &input, switchers[mode].cb_data );

            // Find next enabled
            if ( retv == NEXT_DIALOG ) {
                mode = ( mode + 1 ) % num_switchers;
            }
            else if ( retv == RELOAD_DIALOG ) {
                // do nothing.
            }
            else if ( retv < MODE_EXIT ) {
                mode = ( retv ) % num_switchers;
            }
            else {
                mode = retv;
            }
        } while ( mode != MODE_EXIT );
    }
    g_free ( input );

    // Cleanup font setup.
    textbox_cleanup ();

    if ( do_fork == TRUE ) {
        exit ( EXIT_SUCCESS );
    }
}

// KeyPress event
static void handle_keypress ( XEvent *ev )
{
    KeySym key = XkbKeycodeToKeysym ( display, ev->xkey.keycode, 0, 0 );

    if ( ( windows_modmask == AnyModifier || ev->xkey.state & windows_modmask ) &&
         key == windows_keysym ) {
        int index = switcher_get ( "window" );
        if ( index >= 0 ) {
            run_switcher ( TRUE, index );
        }
    }

    if ( ( rundialog_modmask == AnyModifier || ev->xkey.state & rundialog_modmask ) &&
         key == rundialog_keysym ) {
        int index = switcher_get ( "run" );
        if ( index >= 0 ) {
            run_switcher ( TRUE, index );
        }
    }

    if ( ( sshdialog_modmask == AnyModifier || ev->xkey.state & sshdialog_modmask ) &&
         key == sshdialog_keysym ) {
        int index = switcher_get ( "ssh" );
        if ( index >= 0 ) {
            run_switcher ( TRUE, index );
        }
    }
}

// convert a Mod+key arg to mod mask and keysym
static void parse_key ( Display *display, char *combo, unsigned int *mod, KeySym *key )
{
    unsigned int modmask = 0;

    if ( strcasestr ( combo, "shift" ) ) {
        modmask |= ShiftMask;
    }

    if ( strcasestr ( combo, "control" ) ) {
        modmask |= ControlMask;
    }

    if ( strcasestr ( combo, "mod1" ) ) {
        modmask |= Mod1Mask;
    }

    if ( strcasestr ( combo, "alt" ) ) {
        modmask |= Mod1Mask;
    }

    if ( strcasestr ( combo, "mod2" ) ) {
        modmask |= Mod2Mask;
    }

    if ( strcasestr ( combo, "mod3" ) ) {
        modmask |= Mod3Mask;
    }

    if ( strcasestr ( combo, "mod4" ) ) {
        modmask |= Mod4Mask;
    }

    if ( strcasestr ( combo, "mod5" ) ) {
        modmask |= Mod5Mask;
    }

    *mod = modmask ? modmask : AnyModifier;

    char i = strlen ( combo );

    while ( i > 0 && !strchr ( "-+", combo[i - 1] ) ) {
        i--;
    }

    KeySym sym = XStringToKeysym ( combo + i );

    if ( sym == NoSymbol || ( !modmask && ( strchr ( combo, '-' ) || strchr ( combo, '+' ) ) ) ) {
        fprintf ( stderr, "sorry, cannot understand key combination: %s\n", combo );
        exit ( EXIT_FAILURE );
    }

    *key = sym;
}

// bind a key combination on a root window, compensating for Lock* states
static void grab_key ( Display *display, unsigned int modmask, KeySym key )
{
    Screen *screen  = DefaultScreenOfDisplay ( display );
    Window root     = RootWindow ( display, XScreenNumberOfScreen ( screen ) );
    KeyCode keycode = XKeysymToKeycode ( display, key );
    XUngrabKey ( display, keycode, AnyModifier, root );

    if ( modmask != AnyModifier ) {
        // bind to combinations of mod and lock masks, so caps and numlock don't confuse people
        XGrabKey ( display, keycode, modmask, root, True, GrabModeAsync, GrabModeAsync );
        XGrabKey ( display, keycode, modmask | LockMask, root, True, GrabModeAsync, GrabModeAsync );

        if ( NumlockMask ) {
            XGrabKey ( display, keycode, modmask | NumlockMask, root, True, GrabModeAsync, GrabModeAsync );
            XGrabKey ( display, keycode, modmask | NumlockMask | LockMask, root, True, GrabModeAsync, GrabModeAsync );
        }
    }
    else{
        // nice simple single key bind
        XGrabKey ( display, keycode, AnyModifier, root, True, GrabModeAsync, GrabModeAsync );
    }
}


#ifdef HAVE_I3_IPC_H
static inline void display_get_i3_path ( Display *display )
{
    Screen *screen = DefaultScreenOfDisplay ( display );
    Window root    = RootWindow ( display, XScreenNumberOfScreen ( screen ) );
    i3_socket_path = window_get_text_prop ( root, netatoms[I3_SOCKET_PATH] );
    config_i3_mode = ( i3_socket_path != NULL ) ? TRUE : FALSE;
}
#endif //HAVE_I3_IPC_H


/**
 * Help function. This calls man.
 */
static void help ()
{
    int code = execlp ( "man", "man", MANPAGE_PATH, NULL );

    if ( code == -1 ) {
        fprintf ( stderr, "Failed to execute man: %s\n", strerror ( errno ) );
    }
}

static void parse_cmd_options ( int argc, char ** argv )
{
    // catch help request
    if ( find_arg ( argc, argv, "-h" ) >= 0 ||
         find_arg ( argc, argv, "-help" ) >= 0 ) {
        help ();
        exit ( EXIT_SUCCESS );
    }

    if ( find_arg ( argc, argv, "-v" ) >= 0 ||
         find_arg ( argc, argv, "-version" ) >= 0 ) {
        fprintf ( stdout, "Version: "VERSION "\n" );
        exit ( EXIT_SUCCESS );
    }

    find_arg_str ( argc, argv, "-switchers", &( config.switchers ) );
    // Parse commandline arguments about the looks.
    find_arg_int ( argc, argv, "-opacity", &( config.window_opacity ) );

    find_arg_int ( argc, argv, "-width", &( config.menu_width ) );

    find_arg_int ( argc, argv, "-lines", &( config.menu_lines ) );
    find_arg_int ( argc, argv, "-columns", &( config.menu_columns ) );

    find_arg_str ( argc, argv, "-font", &( config.menu_font ) );
    find_arg_str ( argc, argv, "-fg", &( config.menu_fg ) );
    find_arg_str ( argc, argv, "-bg", &( config.menu_bg ) );
    find_arg_str ( argc, argv, "-hlfg", &( config.menu_hlfg ) );
    find_arg_str ( argc, argv, "-hlbg", &( config.menu_hlbg ) );
    find_arg_str ( argc, argv, "-bc", &( config.menu_bc ) );
    find_arg_int ( argc, argv, "-bw", &( config.menu_bw ) );

    // Parse commandline arguments about size and position
    find_arg_int ( argc, argv, "-location", &( config.location ) );
    find_arg_int ( argc, argv, "-padding", &( config.padding ) );
    find_arg_int ( argc, argv, "-xoffset", &( config.x_offset ) );
    find_arg_int ( argc, argv, "-yoffset", &( config.y_offset ) );
    if ( find_arg ( argc, argv, "-fixed-num-lines" ) >= 0 ) {
        config.fixed_num_lines = 1;
    }
    if ( find_arg ( argc, argv, "-disable-history" ) >= 0 ) {
        config.disable_history = TRUE;
    }
    if ( find_arg ( argc, argv, "-levenshtein-sort" ) >= 0 ) {
        config.levenshtein_sort = TRUE;
    }

    // Parse commandline arguments about behavior
    find_arg_str ( argc, argv, "-terminal", &( config.terminal_emulator ) );
    if ( find_arg ( argc, argv, "-zeltak" ) >= 0 ) {
        config.zeltak_mode = 1;
    }

    if ( find_arg ( argc, argv, "-hmode" ) >= 0 ) {
        config.hmode = TRUE;
    }

    if ( find_arg ( argc, argv, "-ssh-set-title" ) >= 0 ) {
        char *value;
        find_arg_str ( argc, argv, "-ssh-set-title", &value );
        if ( strcasecmp ( value, "true" ) == 0 ) {
            config.ssh_set_title = TRUE;
        }
        else{
            config.ssh_set_title = FALSE;
        }
    }

    // Keybindings
    find_arg_str ( argc, argv, "-key", &( config.window_key ) );
    find_arg_str ( argc, argv, "-rkey", &( config.run_key ) );
    find_arg_str ( argc, argv, "-skey", &( config.ssh_key ) );


    // Dump.
    if ( find_arg ( argc, argv, "-dump-xresources" ) >= 0 ) {
        xresource_dump ();
        exit ( EXIT_SUCCESS );
    }
}

static void cleanup ()
{
    // Cleanup
    if ( display != NULL ) {
        if ( main_window != None ) {
            XFreeGC ( display, gc );
            XDestroyWindow ( display, main_window );
            XCloseDisplay ( display );
        }
    }
    if ( cache_xattr != NULL ) {
        winlist_free ( cache_xattr );
    }
    if ( cache_client != NULL ) {
        winlist_free ( cache_client );
    }
#ifdef HAVE_I3_IPC_H

    if ( i3_socket_path != NULL ) {
        g_free ( i3_socket_path );
    }

#endif

    // Cleaning up memory allocated by the Xresources file.
    // TODO, not happy with this.
    parse_xresource_free ();

    // Whipe the handle.. (not working)
    xdgWipeHandle ( &xdg_handle );

    for ( unsigned int i = 0; i < num_switchers; i++ ) {
        // only used for script dialog.
        if ( switchers[i].cb_data != NULL ) {
            script_switcher_free_options ( switchers[i].cb_data );
        }
    }
    g_free ( switchers );
}

/**
 * Do some input validation, especially the first few could break things.
 * It is good to catch them beforehand.
 *
 * This functions exits the program with 1 when it finds an invalid configuration.
 */
static void config_sanity_check ( void )
{
    if ( config.menu_lines == 0 ) {
        fprintf ( stderr, "config.menu_lines is invalid. You need at least one visible line.\n" );
        exit ( 1 );
    }
    if ( config.menu_columns == 0 ) {
        fprintf ( stderr, "config.menu_columns is invalid. You need at least one visible column.\n" );
        exit ( 1 );
    }
    if ( config.menu_width == 0 ) {
        fprintf ( stderr, "config.menu_width is invalid. You cannot have a window with no width.\n" );
        exit ( 1 );
    }
    if ( !( config.location >= WL_CENTER && config.location <= WL_WEST ) ) {
        fprintf ( stderr, "config.location is invalid. ( %d >= %d >= %d) does not hold.\n",
                  WL_WEST, config.location, WL_CENTER );
        exit ( 1 );
    }
    if ( !( config.hmode == TRUE || config.hmode == FALSE ) ) {
        fprintf ( stderr, "config.hmode is invalid.\n" );
        exit ( 1 );
    }
}

static void setup_switchers ( void )
{
    char *savept;
    char *switcher_str = g_strdup ( config.switchers );
    char *token;
    for ( token = strtok_r ( switcher_str, ",", &savept );
          token != NULL;
          token = strtok_r ( NULL, ",", &savept ) ) {
        if ( strcasecmp ( token, "window" ) == 0 ) {
            switchers = (Switcher *) g_realloc ( switchers, sizeof ( Switcher ) * ( num_switchers + 1 ) );
            copy_string ( switchers[num_switchers].name, "window", 32 );
            switchers[num_switchers].cb      = run_switcher_window;
            switchers[num_switchers].cb_data = NULL;
            num_switchers++;
        }
        else if ( strcasecmp ( token, "ssh" ) == 0 ) {
            switchers = (Switcher *) g_realloc ( switchers, sizeof ( Switcher ) * ( num_switchers + 1 ) );
            copy_string ( switchers[num_switchers].name, "ssh", 32 );
            switchers[num_switchers].cb      = ssh_switcher_dialog;
            switchers[num_switchers].cb_data = NULL;
            num_switchers++;
        }
        else if ( strcasecmp ( token, "run" ) == 0 ) {
            switchers = (Switcher *) g_realloc ( switchers, sizeof ( Switcher ) * ( num_switchers + 1 ) );
            copy_string ( switchers[num_switchers].name, "run", 32 );
            switchers[num_switchers].cb      = run_switcher_dialog;
            switchers[num_switchers].cb_data = NULL;
            num_switchers++;
        }
        else {
            ScriptOptions *sw = script_switcher_parse_setup ( token );
            if ( sw != NULL ) {
                switchers = (Switcher *) g_realloc ( switchers, sizeof ( Switcher ) * ( num_switchers + 1 ) );
                copy_string ( switchers[num_switchers].name, sw->name, 32 );
                switchers[num_switchers].cb      = script_switcher_dialog;
                switchers[num_switchers].cb_data = sw;
                num_switchers++;
            }
            else{
                fprintf ( stderr, "Invalid script switcher: %s\n", token );
                token = NULL;
            }
        }
    }

    g_free ( switcher_str );
}


int main ( int argc, char *argv[] )
{
    // Initialize xdg, so we can grab the xdgCacheHome
    if ( xdgInitHandle ( &xdg_handle ) == NULL ) {
        fprintf ( stderr, "Failed to initialize XDG\n" );
        return EXIT_FAILURE;
    }

    // Get the path to the cache dir.
    cache_dir = xdgCacheHome ( &xdg_handle );

    // Register cleanup function.
    atexit ( cleanup );

    // Get DISPLAY
    char *display_str = getenv ( "DISPLAY" );
    find_arg_str ( argc, argv, "-display", &display_str );

    if ( !( display = XOpenDisplay ( display_str ) ) ) {
        fprintf ( stderr, "cannot open display!\n" );
        return EXIT_FAILURE;
    }

    // Load in config from X resources.
    parse_xresource_options ( display );

    // Parse command line for settings.
    parse_cmd_options ( argc, argv );

    // Sanity check
    config_sanity_check ();

    // setup_switchers
    setup_switchers ();

    // Set up X interaction.
    signal ( SIGCHLD, catch_exit );

    // Set error handle
    XSync ( display, False );
    xerror = XSetErrorHandler ( display_oops );
    XSync ( display, False );

    // determine numlock mask so we can bind on keys with and without it
    XModifierKeymap *modmap = XGetModifierMapping ( display );

    for ( int i = 0; i < 8; i++ ) {
        for ( int j = 0; j < ( int ) modmap->max_keypermod; j++ ) {
            if ( modmap->modifiermap[i * modmap->max_keypermod + j] == XKeysymToKeycode ( display, XK_Num_Lock ) ) {
                NumlockMask = ( 1 << i );
            }
        }
    }



    XFreeModifiermap ( modmap );

    cache_client = winlist_new ();
    cache_xattr  = winlist_new ();

    // X atom values
    for ( int i = 0; i < NUM_NETATOMS; i++ ) {
        netatoms[i] = XInternAtom ( display, netatom_names[i], False );
    }

#ifdef HAVE_I3_IPC_H
    // Check for i3
    display_get_i3_path ( display );
#endif


    // flags to run immediately and exit
    char *sname = NULL;
    if ( find_arg_str ( argc, argv, "-show", &sname ) == TRUE ) {
        int index = switcher_get ( sname );
        if ( index >= 0 ) {
            run_switcher ( FALSE, index );
        }
        else {
            fprintf ( stderr, "The %s switcher has not been enabled\n", sname );
        }
    }
    // Old modi.
    else if ( find_arg ( argc, argv, "-now" ) >= 0 ) {
        int index = switcher_get ( "window" );
        if ( index >= 0 ) {
            run_switcher ( FALSE, index );
        }
        else {
            fprintf ( stderr, "The window switcher has not been enabled\n" );
        }
    }
    else if ( find_arg ( argc, argv, "-rnow" ) >= 0 ) {
        int index = switcher_get ( "run" );
        if ( index >= 0 ) {
            run_switcher ( FALSE, index );
        }
        else {
            fprintf ( stderr, "The run dialog has not been enabled\n" );
        }
    }
    else if ( find_arg ( argc, argv, "-snow" ) >= 0 ) {
        int index = switcher_get ( "ssh" );
        if ( index >= 0 ) {
            run_switcher ( FALSE, index );
        }
        else {
            fprintf ( stderr, "The ssh dialog has not been enabled\n" );
        }
    }
    else if ( find_arg ( argc, argv, "-dmenu" ) >= 0 ) {
        find_arg_str ( argc, argv, "-p", &dmenu_prompt );
        int retv = run_dmenu ();
        // User cancelled the operation.
        if ( retv == FALSE ) {
            return EXIT_FAILURE;
        }
    }
    else{
        // Daemon mode, Listen to key presses..
        if ( switcher_get ( "window" ) >= 0 ) {
            parse_key ( display, config.window_key, &windows_modmask, &windows_keysym );
            grab_key ( display, windows_modmask, windows_keysym );
        }

        if ( switcher_get ( "run" ) >= 0 ) {
            parse_key ( display, config.run_key, &rundialog_modmask, &rundialog_keysym );
            grab_key ( display, rundialog_modmask, rundialog_keysym );
        }

        if ( switcher_get ( "ssh" ) >= 0 ) {
            parse_key ( display, config.ssh_key, &sshdialog_modmask, &sshdialog_keysym );
            grab_key ( display, sshdialog_modmask, sshdialog_keysym );
        }

        // Main loop
        for (;; ) {
            XEvent ev;
            // caches only live for a single event
            winlist_empty ( cache_xattr );
            winlist_empty ( cache_client );

            // block and wait for something
            XNextEvent ( display, &ev );

            if ( ev.xany.window == None ) {
                continue;
            }

            if ( ev.type == KeyPress ) {
                handle_keypress ( &ev );
            }
        }
    }

    return EXIT_SUCCESS;
}
