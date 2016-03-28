////////////////////////////////////////////////////////////////////////////////
// vterm.cc
// author: jcramb@gmail.com

#include <termios.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <utmp.h>
#include <pwd.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <glib.h>
#include <curses.h>

#include "vterm.h"

struct vterm_cell_t {
   chtype         ch;                           // cell data
   guint          attr;                         // cell attributes
};

struct vterm_t {
    gint            rows,cols;                 // terminal height & width
    WINDOW         *window;                    // curses window
    vterm_cell_t  **cells;
    gchar           ttyname[96];               // populated with ttyname_r()
    guint           curattr;                   // current attribute set
    gint            crow,ccol;                 // current cursor column & row
    gint            scroll_min;                // top of scrolling region
    gint            scroll_max;                // bottom of scrolling region
    gint            saved_x,saved_y;           // saved cursor coords
    short           colors;                    // color pair for default fg/bg
    gint            fg,bg;                     // current fg/bg colors
    gchar           esbuf[ESEQ_BUF_SIZE];      /* 0-terminated string. Does
                                                  NOT include the initial
                                                  escape (\x1B) character.   */
    gint            esbuf_len;                 /* length of buffer. The
                                                  following property is
                                                  always kept:
                                                  esbuf[esbuf_len] == '\0'   */
    gint            pty_fd;                    /* file descriptor for the pty
                                                  attached to this terminal. */
    pid_t           child_pid;                 // pid of the child process
    guint           flags;                     // user options
    guint           state;                     // internal state control
    void            (*write) (vterm_t*,guint32);
};

vterm_t* vterm_create(guint width,guint height,guint flags)
{
   vterm_t        *vterm;
   struct passwd  *user_profile;
   char           *user_shell=NULL;
   pid_t          child_pid;
   int            master_fd;
   //int            cell_count;
   int            i;

   if(height <= 0 || width <= 0) return NULL;

   vterm=(vterm_t*)g_malloc0(sizeof(vterm_t));

   /* record dimensions */
   vterm->rows=height;
   vterm->cols=width;

   //cell_count=width*height;

   /* create the cell matrix */
   vterm->cells=(vterm_cell_t**)g_malloc0(sizeof(vterm_cell_t*)*height);

   for(i=0;i < height;i++)
   {
      vterm->cells[i]=(vterm_cell_t*)g_malloc0(sizeof(vterm_cell_t)*width);
   }

   // initialize all cells with defaults
   vterm_erase(vterm);

   // initialization of other public fields
   vterm->crow=0;
   vterm->ccol=0;

   // default active colors
   vterm->curattr=COLOR_PAIR(vterm->colors);

   // initial scrolling area is the whole window
   vterm->scroll_min=0;
   vterm->scroll_max=height-1;

   vterm->flags=flags;

   if(flags & VTERM_FLAG_VT100) vterm->write=vterm_write_vt100;
   else vterm->write=vterm_write_rxvt;
   return vterm;
}

void vterm_destroy(vterm_t *vterm)
{
   int   i;

   if(vterm==NULL) return;

   for(i=0;i < vterm->rows;i++) g_free(vterm->cells[i]);
   g_free(vterm->cells);

   g_free(vterm);

   return;
}

pid_t vterm_get_pid(vterm_t *vterm)
{
   if(vterm==NULL) return -1;

   return vterm->child_pid;
}

gint vterm_get_pty_fd(vterm_t *vterm)
{
   if(vterm==NULL) return -1;

   return vterm->pty_fd;
}

const gchar* vterm_get_ttyname(vterm_t *vterm)
{
   if(vterm == NULL) return NULL;

   return (const gchar*)vterm->ttyname;
}

// WINDOW

void vterm_wnd_set(vterm_t *vterm,WINDOW *window)
{
   if(vterm==NULL) return;

   vterm->window=window;

   return;
}

WINDOW* vterm_wnd_get(vterm_t *vterm)
{
   return vterm->window;
}

void vterm_wnd_update(vterm_t *vterm)
{
   int      i;
   int      x,y;
   int      cell_count;
   chtype   ch;
   guint    attr;

   if(vterm==NULL) return;
   if(vterm->window==NULL) return;

   cell_count=vterm->rows*vterm->cols;

   for(i=0;i < cell_count;i++)
   {
      x=i%vterm->cols;
      y=(int)(i/vterm->cols);

      ch=vterm->cells[y][x].ch;
      attr=vterm->cells[y][x].attr;

      wattrset(vterm->window,attr);
      wmove(vterm->window,y,x);
      waddch(vterm->window,ch);
   }

   if(!(vterm->state & STATE_CURSOR_INVIS))
   {
      mvwchgat(vterm->window,vterm->crow,vterm->ccol,1,A_REVERSE,
         vterm->colors,NULL);
   }

   return;
}

bool validate_escape_suffix(char c)
{
   if(c >= 'a' && c <= 'z') return TRUE;
   if(c >= 'A' && c <= 'Z') return TRUE;
   if(c == '@') return TRUE;
   if(c == '`') return TRUE;

   return FALSE;
}


void clamp_cursor_to_bounds(vterm_t *vterm)
{
   if(vterm->crow < 0) vterm->crow=0;

   if(vterm->ccol < 0) vterm->ccol=0;

   if(vterm->crow >= vterm->rows) vterm->crow=vterm->rows-1;

   if(vterm->ccol >= vterm->cols) vterm->ccol=vterm->cols-1;

   return;
}

void vterm_write_pipe(vterm_t *vterm,guint32 keycode)
{
   if(vterm == NULL) return;

   vterm->write(vterm,keycode);

   return;
}

void vterm_write_rxvt(vterm_t *vterm,guint32 keycode)
{
   gchar *buffer=NULL;

   switch(keycode)
   {
      case '\n':           buffer="\r";      break;
      case KEY_UP:         buffer="\e[A";    break;
      case KEY_DOWN:       buffer="\e[B";    break;
      case KEY_RIGHT:      buffer="\e[C";    break;
      case KEY_LEFT:       buffer="\e[D";    break;
      case KEY_BACKSPACE:  buffer="\b";      break;
      case KEY_IC:         buffer="\e[2~";   break;
      case KEY_DC:         buffer="\e[3~";   break;
      case KEY_HOME:       buffer="\e[7~";   break;
      case KEY_END:        buffer="\e[8~";   break;
      case KEY_PPAGE:      buffer="\e[5~";   break;
      case KEY_NPAGE:      buffer="\e[6~";   break;
      case KEY_SUSPEND:    buffer="\x1A";    break;      // ctrl-z
      case KEY_F(1):       buffer="\e[11~";  break;
      case KEY_F(2):       buffer="\e[12~";  break;
      case KEY_F(3):       buffer="\e[13~";  break;
      case KEY_F(4):       buffer="\e[14~";  break;
      case KEY_F(5):       buffer="\e[15~";  break;
      case KEY_F(6):       buffer="\e[17~";  break;
      case KEY_F(7):       buffer="\e[18~";  break;
      case KEY_F(8):       buffer="\e[19~";  break;
      case KEY_F(9):       buffer="\e[20~";  break;
      case KEY_F(10):      buffer="\e[21~";  break;
      case KEY_F(11):      buffer="\e[23~";  break;
      case KEY_F(12):      buffer="\e[24~";  break;
   }

   if(buffer==NULL)
      write(vterm->pty_fd,&keycode,sizeof(char));
   else
      write(vterm->pty_fd,buffer,strlen(buffer));

   return;
}

void vterm_write_vt100(vterm_t *vterm, guint32 keycode)
{
    gchar *buffer=NULL;

    switch(keycode)
    {
        case '\n':           buffer="\r";      break;
        case KEY_UP:         buffer="\e[A";    break;
        case KEY_DOWN:       buffer="\e[B";    break;
        case KEY_RIGHT:      buffer="\e[C";    break;
        case KEY_LEFT:       buffer="\e[D";    break;
        case KEY_BACKSPACE:  buffer="\b";      break;
        case KEY_IC:         buffer="\e[2~";   break;
        case KEY_DC:         buffer="\e[3~";   break;
        case KEY_HOME:       buffer="\e[7~";   break;
        case KEY_END:        buffer="\e[8~";   break;
        case KEY_PPAGE:      buffer="\e[5~";   break;
        case KEY_NPAGE:      buffer="\e[6~";   break;
        case KEY_SUSPEND:    buffer="\x1A";    break;      // ctrl-z
        case KEY_F(1):       buffer="\e[[A";   break;
        case KEY_F(2):       buffer="\e[[B";   break;
        case KEY_F(3):       buffer="\e[[C";   break;
        case KEY_F(4):       buffer="\e[[D";   break;
        case KEY_F(5):       buffer="\e[[E";   break;
        case KEY_F(6):       buffer="\e[17~";  break;
        case KEY_F(7):       buffer="\e[18~";  break;
        case KEY_F(8):       buffer="\e[19~";  break;
        case KEY_F(9):       buffer="\e[20~";  break;
        case KEY_F(10):      buffer="\e[21~";  break;
    }

    if (buffer == NULL) {
        write(vterm->pty_fd, &keycode, sizeof(char));
    } else {
        write(vterm->pty_fd, buffer, strlen(buffer));
    }
    return;
}

void vterm_remote_read(vterm_t * vterm, const char * buf, int len) {
    vterm_render(vterm, buf, len);
}

void vterm_put_char(vterm_t *vterm,chtype c)
{
	static char		vt100_acs[]="`afgjklmnopqrstuvwxyz{|}~";

   if(vterm->ccol >= vterm->cols)
   {
      vterm->ccol=0;
      vterm_scroll_down(vterm);
   }

   if(IS_MODE_ACS(vterm))
   {
	   if(strchr(vt100_acs,(char)c)!=NULL)
      {
         vterm->cells[vterm->crow][vterm->ccol].ch=NCURSES_ACS(c);
      }
   }
   else
   {
      vterm->cells[vterm->crow][vterm->ccol].ch=c;
   }

   vterm->cells[vterm->crow][vterm->ccol].attr=vterm->curattr;
   vterm->ccol++;

   return;
}

void vterm_render_ctrl_char(vterm_t *vterm,char c)
{
   switch(c)
   {
      /* carriage return */
      case '\r':
      {
         vterm->ccol=0;
         break;
      }

      /* line-feed */
      case '\n':
      {
         vterm->ccol=0;
         vterm_scroll_down(vterm);
         break;
      }

      /* backspace */
      case '\b':
      {
         if(vterm->ccol > 0) vterm->ccol--;
         break;
      }

      /* tab */
      case '\t':
      {
         while(vterm->ccol%8) vterm_put_char(vterm,' ');
         break;
      }

      /* begin escape sequence (aborting previous one if any) */
      case '\x1B':
      {
         vterm_escape_start(vterm);
         break;
      }

      /* enter graphical character mode */
      case '\x0E':
      {
         vterm->state |= STATE_ALT_CHARSET;
        	break;
      }

      /* exit graphical character mode */
      case '\x0F':
      {
         vterm->state &= ~STATE_ALT_CHARSET;
         break;
      }

      /* CSI character. Equivalent to ESC [ */
      case '\x9B':
      {
         vterm_escape_start(vterm);
         vterm->esbuf[vterm->esbuf_len++]='[';
         break;
      }

      /* these interrupt escape sequences */
      case '\x18':
      case '\x1A':
      {
         vterm_escape_cancel(vterm);
         break;
      }

      /* bell */
      case '\a':
      {
         beep();
         break;
      }

#ifdef DEBUG
      default:
         fprintf(stderr, "Unrecognized control char: %d (^%c)\n", c, c + '@');
         break;
#endif
   }
}

void vterm_render(vterm_t *vterm,const char *data,int len)
{
   int i;

   for (i = 0; i < len; i++, data++)
   {
      /* completely ignore NUL */
      if(*data == 0) continue;

      if(*data >= 1 && *data <= 31)
      {
         vterm_render_ctrl_char(vterm,*data);
         continue;
      }

      if(IS_MODE_ESCAPED(vterm) && vterm->esbuf_len < ESEQ_BUF_SIZE)
      {
         /* append character to ongoing escape sequence */
         vterm->esbuf[vterm->esbuf_len]=*data;
         vterm->esbuf[++vterm->esbuf_len]=0;

         try_interpret_escape_seq(vterm);
      }
      else
      {
        	vterm_put_char(vterm,*data);
      }
   }
}

void vterm_escape_start(vterm_t *vterm)
{
   vterm->state |= STATE_ESCAPE_MODE;
   vterm->esbuf_len=0;
   vterm->esbuf[0]='\0';

   return;
}

void vterm_escape_cancel(vterm_t *vterm)
{
   vterm->state &= ~STATE_ESCAPE_MODE;
   vterm->esbuf_len=0;
   vterm->esbuf[0]='\0';

   return;
}

void try_interpret_escape_seq(vterm_t *vterm)
{
   char  firstchar=vterm->esbuf[0];
   char  lastchar=vterm->esbuf[vterm->esbuf_len-1];

   /* too early to do anything */
   if(!firstchar) return;

   /* interpret ESC-M as reverse line-feed */
   if(firstchar=='M')
   {
      vterm_scroll_up(vterm);
      vterm_escape_cancel(vterm);
      return;
   }

   if(firstchar != '[' && firstchar != ']')
   {
      vterm_escape_cancel(vterm);
      return;
   }

   /* we have a csi escape sequence: interpret it */
   if(firstchar == '[' && validate_escape_suffix(lastchar))
   {
      vterm_interpret_csi(vterm);
      vterm_escape_cancel(vterm);
   }
   /* we have an xterm escape sequence: interpret it */
   else if(firstchar == ']' && lastchar == '\a')
   {
      vterm_escape_cancel(vterm);
   }

   /* if the escape sequence took up all available space and could
    * not yet be parsed, abort it */
   if(vterm->esbuf_len + 1 >= ESEQ_BUF_SIZE) vterm_escape_cancel(vterm);

   return;
}

void vterm_erase(vterm_t *vterm)
{
   int   cell_count;
   int   x,y;
   int   i;

   if(vterm == NULL) return;

   cell_count=vterm->rows*vterm->cols;

   for(i=0;i < cell_count;i++)
   {
      x=i%vterm->cols;
      y=(int)(i/vterm->cols);
      vterm->cells[y][x].ch=0x20;
      vterm->cells[y][x].attr=COLOR_PAIR(vterm->colors);
   }

   return;
}

void vterm_erase_row(vterm_t *vterm,gint row)
{
   gint  i;

   if(vterm == NULL) return;

   if(row == -1) row=vterm->crow;

   for(i=0;i < vterm->cols;i++)
   {
      vterm->cells[row][i].ch=0x20;
      vterm->cells[row][i].attr=COLOR_PAIR(vterm->colors);
   }

   return;
}

void vterm_erase_rows(vterm_t *vterm,gint start_row)
{
   if(vterm == NULL) return;
   if(start_row < 0) return;

   while(start_row < vterm->rows)
   {
      vterm_erase_row(vterm,start_row);
      start_row++;
   }

   return;
}

void vterm_erase_col(vterm_t *vterm,gint col)
{
   gint  i;

   if(vterm==NULL) return;

   if(col==-1) col=vterm->ccol;

   for(i=0;i < vterm->rows;i++)
   {
      vterm->cells[i][col].ch=0x20;
      vterm->cells[i][col].attr=COLOR_PAIR(vterm->colors);
   }

   return;
}

void vterm_erase_cols(vterm_t *vterm,gint start_col)
{
   if(vterm == NULL) return;
   if(start_col < 0) return;

   while(start_col < vterm->cols)
   {
      vterm_erase_col(vterm,start_col);
      start_col++;
   }

   return;
}

void vterm_resize(vterm_t *vterm,guint width,guint height)
{
    struct winsize ws;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
   guint          i;
   gint           delta_x;
   gint           delta_y;
   gint           start_x;
   gint           start_y;

   if(vterm==NULL) return;
   if(width==0 || height==0) return;

   delta_x=width-vterm->cols;
   delta_y=height-vterm->rows;
   start_x=vterm->cols;
   start_y=vterm->rows;

   vterm->cells=(vterm_cell_t**)g_realloc(vterm->cells,
      sizeof(vterm_cell_t*)*height);

   for(i=0;i < height;i++)
   {
      // realloc() does not initlize new elements
      if((delta_y > 0) && (i > vterm->rows-1)) vterm->cells[i]=NULL;

      vterm->cells[i]=(vterm_cell_t*)g_realloc(vterm->cells[i],
         sizeof(vterm_cell_t)*width);
   }

   vterm->rows=height;
   vterm->cols=width;
   if(!(vterm->state & STATE_SCROLL_SHORT))
   {
      vterm->scroll_max=height-1;
   }

   ws.ws_row=height;
   ws.ws_col=width;

   clamp_cursor_to_bounds(vterm);

   if(delta_x > 0) vterm_erase_cols(vterm,start_x);
   if(delta_y > 0) vterm_erase_rows(vterm,start_y);

   ioctl(vterm->pty_fd,TIOCSWINSZ,&ws);
   kill(vterm->child_pid,SIGWINCH);

   return;
}

void vterm_scroll_down(vterm_t *vterm)
{
   int i;

   vterm->crow++;

   if(vterm->crow <= vterm->scroll_max) return;

   /* must scroll the scrolling region up by 1 line, and put cursor on 
    * last line of it */
   vterm->crow=vterm->scroll_max;

   for(i=vterm->scroll_min; i < vterm->scroll_max; i++)
   {
      // vterm->dirty_lines[i]=TRUE;
      memcpy(vterm->cells[i],vterm->cells[i+1],
         sizeof(vterm_cell_t)*vterm->cols);
   }

   /* clear last row of the scrolling region */
   vterm_erase_row(vterm,vterm->scroll_max);

   return;
}

void vterm_scroll_up(vterm_t *vterm)
{
   int i;

   vterm->crow--;

   if(vterm->crow >= vterm->scroll_min) return;

   /* must scroll the scrolling region up by 1 line, and put cursor on 
    * first line of it */
   vterm->crow=vterm->scroll_min;

   for(i=vterm->scroll_max;i > vterm->scroll_min;i--)
   {
      // vterm->dirty_lines[i]=TRUE;
      memcpy(vterm->cells[i],vterm->cells[i-1],
         sizeof(vterm_cell_t)*vterm->cols);
   }

   /* clear first row of the scrolling region */
   vterm_erase_row(vterm,vterm->scroll_min);

   return;
}

//////////////////////////////////////////////////////////////////////////
// CSI

void vterm_interpret_csi(vterm_t *vterm) {
   static int csiparam[MAX_CSI_ES_PARAMS];
   int param_count = 0;
   const char * p;
   char verb;

   p = vterm->esbuf+1;
   verb = vterm->esbuf[vterm->esbuf_len-1];

   // parse numeric parameters 
   while ((*p >= '0' && *p <= '9') || *p == ';' || *p == '?') {
      if (*p == '?') {
         p++;
         continue;
      }
      if(*p == ';') {
         if (param_count >= MAX_CSI_ES_PARAMS) return; // too long
         csiparam[param_count++]=0;
      } else {
         if (param_count==0) csiparam[param_count++] = 0;
         csiparam[param_count-1] *= 10;
         csiparam[param_count-1] += *p - '0';
      }
      p++;
   }

   // delegate handling depending on command character (verb)
   switch (verb) {
      case 'm': interpret_csi_SGR(vterm, csiparam, param_count); break;
      case 'l': interpret_dec_RM(vterm, csiparam, param_count); break;
      case 'h': interpret_dec_SM(vterm, csiparam, param_count); break;
      case 'J': interpret_csi_ED(vterm, csiparam, param_count); break;
      case 'H':
      case 'f': interpret_csi_CUP(vterm, csiparam, param_count); break;
      case 'A':
      case 'B':
      case 'C':
      case 'D':
      case 'E':
      case 'F':
      case 'G':
      case 'e':
      case 'a':
      case 'd':
      case '`': interpret_csi_CUx(vterm, verb, csiparam, param_count); break;
      case 'K': interpret_csi_EL(vterm, csiparam, param_count); break;
      case '@': interpret_csi_ICH(vterm, csiparam, param_count); break;
      case 'P': interpret_csi_DCH(vterm, csiparam, param_count); break;
      case 'L': interpret_csi_IL(vterm, csiparam, param_count); break;
      case 'M': interpret_csi_DL(vterm, csiparam, param_count); break;
      case 'X': interpret_csi_ECH(vterm, csiparam, param_count); break;
      case 'r': interpret_csi_DECSTBM(vterm, csiparam, param_count); break;
      case 's': interpret_csi_SAVECUR(vterm, csiparam, param_count); break;
      case 'u': interpret_csi_RESTORECUR(vterm, csiparam, param_count); break;
#ifdef DEBUG
      default:
         fprintf(stderr, "Unrecogized CSI: <%s>\n", rt->pd->esbuf); break;
#endif
   }
}

/* interprets a 'move cursor' (CUP) escape sequence */
void interpret_csi_CUP(vterm_t *vterm, int param[], int pcount)
{
   if (pcount == 0)
   {
      /* special case */
      vterm->crow=0;
      vterm->ccol=0;
      return;
   }
   else if (pcount < 2) return;  // malformed

   vterm->crow=param[0]-1;       // convert from 1-based to 0-based.
   vterm->ccol=param[1]-1;       // convert from 1-based to 0-based.

   // vterm->state |= STATE_DIRTY_CURSOR;

   clamp_cursor_to_bounds(vterm);
}

/* Interpret the 'relative mode' sequences: CUU, CUD, CUF, CUB, CNL,
 * CPL, CHA, HPR, VPA, VPR, HPA */
void interpret_csi_CUx(vterm_t *vterm,char verb,int param[],int pcount)
{
   int n=1;

   if(pcount && param[0]>0) n=param[0];

   switch (verb)
   {
      case 'A':         vterm->crow -= n;          break;
      case 'B':
      case 'e':         vterm->crow += n;          break;
      case 'C':
      case 'a':         vterm->ccol += n;          break;
      case 'D':         vterm->ccol -= n;          break;
      case 'E':
      {
         vterm->crow += n;
         vterm->ccol = 0;
         break;
      }
      case 'F':
      {
         vterm->crow -= n;
         vterm->ccol = 0;
         break;
      }
      case 'G':
      case '`':         vterm->ccol=param[0]-1;    break;
      case 'd':         vterm->crow=param[0]-1;    break;
   }

   // vterm->state |= STATE_DIRTY_CURSOR;
   clamp_cursor_to_bounds(vterm);

   return;
}

/* Interpret the 'delete chars' sequence (DCH) */
void interpret_csi_DCH(vterm_t *vterm, int param[], int pcount)
{
   int i;
   int n=1;

   if(pcount && param[0] > 0) n=param[0]; 

   for(i=vterm->ccol;i < vterm->cols;i++)
   {
      if(i+n < vterm->cols)
      {
         vterm->cells[vterm->crow][i]=vterm->cells[vterm->crow][i+n];
      }
      else
      {
         vterm->cells[vterm->crow][i].ch=0x20;
         vterm->cells[vterm->crow][i].attr=vterm->curattr;
      }
   }
}

/* Interpret a 'set scrolling region' (DECSTBM) sequence */
void interpret_csi_DECSTBM(vterm_t *vterm,int param[],int pcount)
{
   int newtop, newbottom;

   if(!pcount)
   {
      newtop=0;
      newbottom=vterm->rows-1;
   }
   else if(pcount < 2) return; /* malformed */
   else
   {
      newtop=param[0]-1;
      newbottom=param[1]-1;
   }

   /* clamp to bounds */
   if(newtop < 0) newtop=0;
   if(newtop >= vterm->rows) newtop=vterm->rows-1;
   if(newbottom < 0) newbottom=0;
   if(newbottom >= vterm->rows) newbottom=vterm->rows-1;

   /* check for range validity */
   if(newtop > newbottom) return;
   vterm->scroll_min=newtop;
   vterm->scroll_max=newbottom;

   if(vterm->scroll_min != 0)
      vterm->state |= STATE_SCROLL_SHORT;

   if(vterm->scroll_max != vterm->rows-1)
      vterm->state |= STATE_SCROLL_SHORT;

   if((vterm->scroll_min == 0) && (vterm->scroll_max == vterm->rows-1))
      vterm->state &= ~STATE_SCROLL_SHORT;

   return;
}

/* Interpret a 'delete line' sequence (DL) */
void interpret_csi_DL(vterm_t *vterm,int param[],int pcount)
{
   int i, j;
   int n=1;

   if(pcount && param[0] > 0) n=param[0];

   for(i=vterm->crow;i <= vterm->scroll_max; i++)
   {
      // vterm->dirty_lines[i]=TRUE;

      if(i+n <= vterm->scroll_max)
      {
         memcpy(vterm->cells[i],vterm->cells[i+n],
            sizeof(vterm_cell_t)*vterm->cols);
      }
      else
      {
         for(j=0;j < vterm->cols;j++)
         {
            vterm->cells[i][j].ch=0x20;
            vterm->cells[i][j].attr=vterm->curattr;
         }
      }
   }

   return;
}

/* Interpret an 'erase characters' (ECH) sequence */
void interpret_csi_ECH(vterm_t *vterm,int param[],int pcount)
{
   int i;
   int n=1;

   if(pcount && param[0] > 0) n=param[0];

   for(i=vterm->ccol;i < vterm->ccol+n; i++)
   {
      if(i >= vterm->cols) break;

      vterm->cells[vterm->crow][i].ch=0x20;
      vterm->cells[vterm->crow][i].attr=vterm->curattr;
   }

   return;
}

/* interprets an 'erase display' (ED) escape sequence */
void interpret_csi_ED(vterm_t *vterm, int param[], int pcount)
{
   int r, c;
   int start_row, start_col, end_row, end_col;

   /* decide range */
   if(pcount && param[0]==2)
   {
      start_row=0;
      start_col=0;
      end_row=vterm->rows-1;
      end_col=vterm->cols-1;
   }
   else if(pcount && param[0]==1)
   {
      start_row=0;
      start_col=0;
      end_row=vterm->crow;
      end_col=vterm->ccol;
   }
   else
   {
      start_row=vterm->crow;
      start_col=vterm->ccol;
      end_row=vterm->rows-1;
      end_col=vterm->cols-1;
   }

   /* clean range */
   for(r=start_row;r <= end_row;r++)
   {
      for(c=start_col;c <= end_col;c++)
      {
         vterm->cells[r][c].ch=0x20;               // erase with blanks.
         vterm->cells[r][c].attr=vterm->curattr;   // set to current attributes.
      }
   }
}

/* Interpret the 'erase line' escape sequence */
void interpret_csi_EL(vterm_t *vterm, int param[], int pcount)
{
   int erase_start, erase_end, i;
   int cmd=0;

   if(pcount>0) cmd=param[0];

   switch(cmd)
   {
      case 1:
      {
         erase_start=0;
         erase_end=vterm->ccol;
         break;
      }
      case 2:
      {
         erase_start=0;
         erase_end=vterm->cols-1;
         break;
      }
      default:
      {
         erase_start=vterm->ccol;
         erase_end=vterm->cols-1;
         break;
      }
   }

   for(i=erase_start;i <= erase_end;i++)
   {
      vterm->cells[vterm->crow][i].ch = 0x20; 
      vterm->cells[vterm->crow][i].attr = vterm->curattr;
   }

   return;
}

/* Interpret the 'insert blanks' sequence (ICH) */
void interpret_csi_ICH(vterm_t *vterm,int param[],int pcount)
{
   int i;
   int n=1;

   if(pcount && param[0]>0) n=param[0];

   for (i=vterm->cols-1;i >= vterm->ccol+n;i--)
   {
      vterm->cells[vterm->crow][i]=vterm->cells[vterm->crow][i-n];
   }

   for(i=vterm->ccol;i < vterm->ccol+n;i++)
   {
      vterm->cells[vterm->crow][i].ch=0x20;
      vterm->cells[vterm->crow][i].attr = vterm->curattr;
   }

   return;
}

/* Interpret an 'insert line' sequence (IL) */
void interpret_csi_IL(vterm_t *vterm,int param[],int pcount)
{
   int i, j;
   int n=1;

   if(pcount && param[0] > 0) n=param[0];

   for(i=vterm->scroll_max;i >= vterm->crow+n;i--)
   {
      memcpy(vterm->cells[i],vterm->cells[i - n],
         sizeof(vterm_cell_t)*vterm->cols);
   }

   for(i=vterm->crow;i < vterm->crow+n; i++)
   {
      if(i>vterm->scroll_max) break;

      // vterm->dirty_lines[i]=TRUE;

      for(j=0;j < vterm->cols; j++)
      {
         vterm->cells[i][j].ch = 0x20;
         vterm->cells[i][j].attr=vterm->curattr;
      }
   }

   return;
}

void interpret_csi_RESTORECUR(vterm_t *vterm,int param[],int pcount)
{
   vterm->ccol=vterm->saved_x;
   vterm->crow=vterm->saved_y;

   return;
}

void interpret_csi_SAVECUR(vterm_t *vterm,int param[],int pcount)
{
   vterm->saved_x=vterm->ccol;
   vterm->saved_y=vterm->crow;

   return;
}

/*
   VT100 SGR documentation
   From http://vt100.net/docs/vt510-rm/SGR table 5-16
   0  All attributes off
   1  Bold
   4  Underline
   5  Blinking
   7  Negative image
   8  Invisible image
   10    The ASCII character set is the current 7-bit
         display character set (default) - SCO Console only.
   11    Map Hex 00-7F of the PC character set codes
         to the current 7-bit display character set
         - SCO Console only.
   12    Map Hex 80-FF of the current character set to
         the current 7-bit display character set - SCO
         Console only.
   22    Bold off
   24    Underline off
   25    Blinking off
   27    Negative image off
   28    Invisible image off
*/

/* interprets a 'set attribute' (SGR) CSI escape sequence */
void interpret_csi_SGR(vterm_t *vterm, int param[], int pcount)
{
   int   i;
   short colors;
   short default_fg,default_bg;

   if(pcount==0)
   {
      vterm->curattr=A_NORMAL;                        // reset attributes
      return;
   }

   for(i=0;i<pcount;i++)
   {
      if(param[i]==0)                                 // reset attributes
      {
         vterm->curattr=A_NORMAL;
         continue;
      }

      if(param[i]==1 || param[i]==2 || param[i]==4)   // bold on
      {
         vterm->curattr |= A_BOLD;
         continue;
      }

      if(param[i]==5)                                 // blink on
      {
         vterm->curattr |= A_BLINK;
         continue;
      }

      if(param[i]==7 || param[i]==27)                 // reverse on
      {
         vterm->curattr |= A_REVERSE;
         continue;
      }

      if(param[i]==8)                                 // invisible on
      {
         vterm->curattr |= A_INVIS;
         continue;
      }

      if(param[i]==10)                                // rmacs
      {
         vterm->state &= ~STATE_ALT_CHARSET;
         continue;
      }

		if(param[i]==11)                                // smacs
      {
         vterm->state |= STATE_ALT_CHARSET;
         continue;
      }

      if(param[i]==22 || param[i]==24)                // bold off
      {
         vterm->curattr &= ~A_BOLD;
         continue;
      }

      if(param[i]==25)                                // blink off
      {
         vterm->curattr &= ~A_BLINK;
         continue;
      }

      if(param[i]==28)                                // invisible off
      {
         vterm->curattr &= ~A_INVIS;
         continue;
      }

      if(param[i] >= 30 && param[i] <= 37)            // set fg color
      {
         vterm->fg=param[i]-30;
         colors=find_color_pair(vterm->fg,vterm->bg);
         if(colors==-1) colors=0;
         vterm->curattr |= COLOR_PAIR(colors);
         continue;
      }

      if(param[i] >= 40 && param[i] <= 47)            // set bg color
      {
         vterm->bg=param[i]-40;
         colors=find_color_pair(vterm->fg,vterm->bg);
         if(colors==-1) colors=0;
         vterm->curattr |= COLOR_PAIR(colors);
         continue;
      }

      if(param[i]==39)                                // reset fg color
      {
         pair_content(vterm->colors,&default_fg,&default_bg);
         vterm->fg=default_fg;
         colors=find_color_pair(vterm->fg,vterm->bg);
         if(colors==-1) colors=0;
         vterm->curattr |= COLOR_PAIR(colors);
         continue;
      }

      if(param[i]==49)                                // reset bg color
      {
         pair_content(vterm->colors,&default_fg,&default_bg);
         vterm->bg=default_bg;
         colors=find_color_pair(vterm->fg,vterm->bg);
         if(colors==-1) colors=0;
         vterm->curattr |= COLOR_PAIR(colors);
         continue;
      }
   }
}

int vterm_set_colors(vterm_t *vterm,short fg,short bg)
{
   short colors;

   if(vterm==NULL) return -1;
   if(has_colors()==FALSE) return -1;

   colors=find_color_pair(fg,bg);
   if(colors==-1) colors=0;

   vterm->colors=colors;

   return 0;
}

short vterm_get_colors(vterm_t *vterm)
{
   if(vterm==NULL) return -1;
   if(has_colors()==FALSE) return -1;

   return vterm->colors;
}

short find_color_pair(short fg,short bg)
{
   short fg_color,bg_color;
   int   i;

   if(has_colors()==FALSE) return -1;

   for(i=1;i<COLOR_PAIRS;i++)
   {
      pair_content(i,&fg_color,&bg_color);
      if(fg_color==fg && bg_color==bg) break;
   }

   if(i==COLOR_PAIRS) return -1;

   return i;
}

/* Interpret DEC SM (set mode) */
void interpret_dec_SM(vterm_t *vterm,int param[],int pcount)
{
   int i;

   if(pcount==0) return;

   for(i=0;i < pcount;i++)
   {
      /* civis is actually "normal" for rxvt */
      if(param[i]==25) vterm->state &= ~STATE_CURSOR_INVIS;
   }
}

/* Interpret the DEC RM (reset mode) */
void interpret_dec_RM(vterm_t *vterm,int param[],int pcount)
{
   int i;

   if(pcount==0) return;

   for(i=0;i < pcount;i++)
   {
      /* civis is actually the "normal" vibility for rxvt   */
      if(param[i]==25) vterm->state |= STATE_CURSOR_INVIS;
   }
}
