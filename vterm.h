////////////////////////////////////////////////////////////////////////////////
// Based on libvterm by 2009 Bryan Christ
// and ROTE written by Bruno Takahashi C. de Oliveira.

#ifndef vterm_h
#define vterm_h

#include <sys/types.h>
#include <unistd.h>
#include <curses.h>
#include <glib.h>

#define LIBVTERM_VERSION "0.99.7"
#define VTERM_FLAG_VT100 (1<<1)
#define ESEQ_BUF_SIZE 128                // size of escape sequence buffer.

#define STATE_ALT_CHARSET     (1<<1)
#define STATE_ESCAPE_MODE     (1<<2)
#define STATE_PIPE_ERR        (1<<3)
#define STATE_CHILD_EXITED    (1<<4)
#define STATE_CURSOR_INVIS    (1<<5)
#define STATE_SCROLL_SHORT    (1<<6)      // scrolling region is not full height

#define IS_MODE_ESCAPED(x)    (x->state & STATE_ESCAPE_MODE)
#define IS_MODE_ACS(x)        (x->state & STATE_ALT_CHARSET)

struct vterm_cell_t;
struct vterm_t;

vterm_t*     vterm_create(guint width, guint height, guint flags);
void         vterm_destroy(vterm_t *vterm);
pid_t        vterm_get_pid(vterm_t *vterm);
int          vterm_get_pty_fd(vterm_t *vterm);
const char*  vterm_get_ttyname(vterm_t *vterm);

void         vterm_remote_read(vterm_t * term, const char * buf, int len);
void         vterm_write_pipe(vterm_t *vterm, guint32 keycode);

void         vterm_wnd_set(vterm_t *vterm,WINDOW *window);
WINDOW*      vterm_wnd_get(vterm_t *vterm);
void         vterm_wnd_update(vterm_t *vterm);

int          vterm_set_colors(vterm_t *vterm, short fg, short bg);
short        vterm_get_colors(vterm_t *vterm);

void         vterm_erase(vterm_t *vterm);
void         vterm_erase_row(vterm_t *vterm,int row);
void         vterm_erase_rows(vterm_t *vterm, int start_row);
void         vterm_erase_col(vterm_t *vterm, int col);
void         vterm_erase_cols(vterm_t *vterm, int start_cols);
void         vterm_scroll_up(vterm_t *vterm);
void         vterm_scroll_down(vterm_t *vterm);

void         vterm_resize(vterm_t *vterm,guint width,guint height);

// private

#define VTERM_CELL(vterm_ptr,x,y)               ((y*vterm_ptr->cols)+x)

bool  validate_escape_suffix(char c);
void  clamp_cursor_to_bounds(vterm_t *vterm);

void  vterm_write_rxvt(vterm_t *vterm,guint32 keycode);
void  vterm_write_vt100(vterm_t *vterm,guint32 keycode);

void vterm_render(vterm_t *,const char *data,int len);
void vterm_put_char(vterm_t *vterm,chtype c);
void vterm_render_ctrl_char(vterm_t *vterm,char c);
void try_interpret_escape_seq(vterm_t *vterm);

// escape
void  vterm_escape_start(vterm_t *vterm);
void  vterm_escape_cancel(vterm_t *vterm);
void  try_interpret_escape_seq(vterm_t *vterm);

// CSI

#define MAX_CSI_ES_PARAMS 32

/* interprets a CSI escape sequence stored in buffer  */
void  vterm_interpret_csi(vterm_t *vterm);

void  interpret_dec_SM(vterm_t *vterm,int param[],int pcount);
void  interpret_dec_RM(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_SGR(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_ED(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_CUP(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_ED(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_EL(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_ICH(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_DCH(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_IL(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_DL(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_ECH(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_CUx(vterm_t *vterm,char verb,int param[],int pcount);
void  interpret_csi_DECSTBM(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_SAVECUR(vterm_t *vterm,int param[],int pcount);
void  interpret_csi_RESTORECUR(vterm_t *vterm,int param[],int pcount);

short find_color_pair(short fg,short bg);

#endif // vterm_h
