/*
  keyboard.c — GLES2 rectangles + GLES2 text labels.
  JSON-driven layout with optional "shift_label".
  Depressed button effect, XTest key injection.
  Supports multiple simultaneous presses.
  Shift, Caps Lock, Ctrl, and Alt toggles with proper logic.
  Shift, Ctrl, and Alt auto-release after next non-modifier key.
  Key repeat for held keys (e.g. Backspace).
  Keys can specify width/height multipliers in JSON.
  Window occupies bottom third of the screen.

  Build:
    gcc keyboard.c -o keyboard -lcjson -lX11 -lXtst -lEGL -lGLESv2 -lm

  Run:
    ./keyboard layout.json
*/

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <cjson/cJSON.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <unistd.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define XK_Preferences 0x1008FF30

bool menu_visible = false;
int menu_pressed = -1; // -1 means none pressed
bool keyboard_visible = false;
int shift_down=0,caps_down=0,ctrl_down=0,alt_down=0,fn_down=0;

typedef struct {
    float x,y,w,h;
    char label[64];
    char shift_label[64];
    KeySym keysym;
} Key;

/* Shader helpers */
static GLuint make_shader(GLenum type,const char*src){
    GLuint s=glCreateShader(type);
    glShaderSource(s,1,&src,NULL);
    glCompileShader(s);
    return s;
}
static GLuint make_program(const char*vs,const char*fs){
    GLuint p=glCreateProgram();
    GLuint v=make_shader(GL_VERTEX_SHADER,vs);
    GLuint f=make_shader(GL_FRAGMENT_SHADER,fs);
    glAttachShader(p,v); glAttachShader(p,f);
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

/* Rect shaders */
static const char* RECT_VS="attribute vec2 aPos;attribute vec3 aCol;varying vec3 vCol;uniform vec2 uRes;void main(){vec2 ndc=(aPos/uRes)*2.0-1.0;gl_Position=vec4(ndc.x,-ndc.y,0.0,1.0);vCol=aCol;}";
static const char* RECT_FS="precision mediump float;varying vec3 vCol;void main(){gl_FragColor=vec4(vCol,1.0);}";

static GLuint rect_prog; static GLint rect_aPos,rect_aCol,rect_uRes;
typedef struct { float x,y,r,g,b; } RectVtx;

/* Text shaders */
static const char* TEXT_VS="attribute vec2 aPos;attribute vec2 aUV;varying vec2 vUV;uniform vec2 uRes;void main(){vec2 ndc=(aPos/uRes)*2.0-1.0;gl_Position=vec4(ndc.x,-ndc.y,0.0,1.0);vUV=aUV;}";
//static const char* TEXT_FS="precision mediump float;varying vec2 vUV;uniform sampler2D uFont;void main(){float a=texture2D(uFont,vUV).a;gl_FragColor=vec4(1.0,1.0,1.0,a);}";

static const char* TEXT_FS =
"precision mediump float;"
"varying vec2 vUV;"
"uniform sampler2D uFont;"
"uniform vec3 uColor;"
"void main(){"
" float a = texture2D(uFont,vUV).a;"
" gl_FragColor = vec4(uColor, a);"
"}";

static GLint text_uColor;
static GLuint text_prog; static GLint text_aPos,text_aUV,text_uRes,text_uFont;
static GLuint fontTex;
static stbtt_bakedchar cdata[96]; // ASCII 32..126

float text_width(const char* s,float scale){
    float w=0.0f;
    for(const char* p=s;*p;p++){
        if(*p<32||*p>=128) continue;
        w+=cdata[*p-32].xadvance*scale;
    }
    return w;
}

/* Draw rectangles */
static void draw_keys(int width,int height,Key*keys,int n,int pressed[],int caps_down){
    glUseProgram(rect_prog);
    glUniform2f(rect_uRes,(float)width,(float)height);
    for(int i=0;i<n;i++){
        float x=keys[i].x,y=keys[i].y,w=keys[i].w,h=keys[i].h;
        float r=0.3f,g=0.3f,b=0.3f;
        int is_pressed = pressed[i];
        if (keys[i].keysym == XK_Caps_Lock && caps_down) is_pressed = 1;
	if (keys[i].keysym == XK_Mode_switch && fn_down) is_pressed = 1;
        if(is_pressed){ r*=0.5f; g*=0.5f; b*=0.5f; }
        RectVtx quad[6]={{x,y,r,g,b},{x+w,y,r,g,b},{x+w,y+h,r,g,b},
                         {x,y,r,g,b},{x+w,y+h,r,g,b},{x,y+h,r,g,b}};
        glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&quad[0].x);
        glEnableVertexAttribArray(rect_aPos);
        glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&quad[0].r);
        glEnableVertexAttribArray(rect_aCol);
        glDrawArrays(GL_TRIANGLES,0,6);
    }
}

/* Draw text with stb_truetype */
static void draw_text(const char* str,float x,float y,float scale,int win_w,int win_h){
    glUseProgram(text_prog);
    glUniform2f(text_uRes,(float)win_w,(float)win_h);
    glUniform1i(text_uFont,0);
    glBindTexture(GL_TEXTURE_2D,fontTex);

    float xpos=x;
    for(const char* p=str;*p;p++){
        if(*p<32||*p>=128) continue;
        stbtt_bakedchar* b=&cdata[*p-32];
        float x0=xpos+b->xoff*scale;
        float y0=y+b->yoff*scale;
        float x1=x0+(b->x1-b->x0)*scale;
        float y1=y0+(b->y1-b->y0)*scale;
        float u0=b->x0/512.0f, v0=b->y0/512.0f;
        float u1=b->x1/512.0f, v1=b->y1/512.0f;

        GLfloat verts[16]={
            x0,y0,u0,v0,
            x1,y0,u1,v0,
            x1,y1,u1,v1,
            x0,y1,u0,v1
        };
        glVertexAttribPointer(text_aPos,2,GL_FLOAT,GL_FALSE,4*sizeof(GLfloat),verts);
        glEnableVertexAttribArray(text_aPos);
        glVertexAttribPointer(text_aUV,2,GL_FLOAT,GL_FALSE,4*sizeof(GLfloat),verts+2);
        glEnableVertexAttribArray(text_aUV);
        glDrawArrays(GL_TRIANGLE_FAN,0,4);
        xpos+=b->xadvance*scale;
    }
}

static void draw_text_colored(const char* str,float x,float y,float scale,
                              int win_w,int win_h,float r,float g,float b){
    glUseProgram(text_prog);
    glUniform2f(text_uRes,(float)win_w,(float)win_h);
    glUniform1i(text_uFont,0);
    glUniform3f(text_uColor,r,g,b);
    glBindTexture(GL_TEXTURE_2D,fontTex);

    float xpos=x;
    for(const char* p=str;*p;p++){
        if(*p<32||*p>=128) continue;
        stbtt_bakedchar* bch=&cdata[*p-32];
        float x0=xpos+bch->xoff*scale;
        float y0=y+bch->yoff*scale;
        float x1=x0+(bch->x1-bch->x0)*scale;
        float y1=y0+(bch->y1-bch->y0)*scale;
        float u0=bch->x0/512.0f, v0=bch->y0/512.0f;
        float u1=bch->x1/512.0f, v1=bch->y1/512.0f;

        GLfloat verts[16]={
            x0,y0,u0,v0,
            x1,y0,u1,v0,
            x1,y1,u1,v1,
            x0,y1,u0,v1
        };
        glVertexAttribPointer(text_aPos,2,GL_FLOAT,GL_FALSE,4*sizeof(GLfloat),verts);
        glEnableVertexAttribArray(text_aPos);
        glVertexAttribPointer(text_aUV,2,GL_FLOAT,GL_FALSE,4*sizeof(GLfloat),verts+2);
        glEnableVertexAttribArray(text_aUV);
        glDrawArrays(GL_TRIANGLE_FAN,0,4);
        xpos+=bch->xadvance*scale;
    }
}

static void draw_key_labels(Key* K, int win_w, int win_h, int shift_down, int caps_down) {
    float white[3] = {1.0f,1.0f,1.0f};
    float grey[3]  = {0.7f,0.7f,0.7f};

    // Helper: is this a letter key (exactly one alphabetic character)?
    bool is_letter = (strlen(K->label) == 1) && isalpha((unsigned char)K->label[0]);

if (fn_down) {
    switch (K->keysym) {
        case XK_1:  draw_text_colored("F1",  K->x+4, K->y+24, 0.8f, win_w, win_h, 1,1,1); return;
        case XK_2:  draw_text_colored("F2",  K->x+4, K->y+24, 0.8f, win_w, win_h, 1,1,1); return;
        case XK_3:  draw_text_colored("F3",  K->x+4, K->y+24, 0.8f, win_w, win_h, 1,1,1); return;
        case XK_4:  draw_text_colored("F4",  K->x+4, K->y+24, 0.8f, win_w, win_h, 1,1,1); return;
        case XK_5:  draw_text_colored("F5",  K->x+4, K->y+24, 0.8f, win_w, win_h, 1,1,1); return;
        case XK_6:  draw_text_colored("F6",  K->x+4, K->y+24, 0.8f, win_w, win_h, 1,1,1); return;
        case XK_7:  draw_text_colored("F7",  K->x+4, K->y+24, 0.8f, win_w, win_h, 1,1,1); return;
        case XK_8:  draw_text_colored("F8",  K->x+4, K->y+24, 0.8f, win_w, win_h, 1,1,1); return;
        case XK_9:  draw_text_colored("F9",  K->x+4, K->y+24, 0.8f, win_w, win_h, 1,1,1); return;
        case XK_0:  draw_text_colored("F10", K->x+4, K->y+24, 0.8f, win_w, win_h, 1,1,1); return;
        case XK_minus: draw_text_colored("F11", K->x+4, K->y+24, 0.8f, win_w, win_h, 1,1,1); return;
        case XK_equal: draw_text_colored("F12", K->x+4, K->y+24, 0.8f, win_w, win_h, 1,1,1); return;
    }
}

    // 1) Letters: single centered label, case toggled by caps ^ shift

/*
    if (is_letter) {
        char buf[2] = {0};
        buf[0] = (caps_down ^ shift_down) ? toupper((unsigned char)K->label[0])
                                          : tolower((unsigned char)K->label[0]);

        float scale = fmaxf(0.8f,(K->h*0.5f)/32.0f);
        float tw = text_width(buf, scale);
        float tx = K->x + (K->w - tw)/2.0f;
        float ty = K->y + K->h*0.65f;
        draw_text_colored(buf, tx, ty, scale, win_w, win_h, white[0], white[1], white[2]);
        return;
    }
*/

if (is_letter) {
    char buf[2] = {0};
    buf[0] = (caps_down ^ shift_down)
               ? toupper((unsigned char)K->label[0])
               : tolower((unsigned char)K->label[0]);

    float scale = fmaxf(0.8f,(K->h*0.5f)/32.0f);
    float tx = K->x + 4.0f;          // left padding
    float ty = K->y + scale * 24.0f; // top padding
    draw_text_colored(buf, tx, ty, scale, win_w, win_h, 1,1,1);
    return;
}

    // 2) Dual-label keys: show both normal (center) and shift (top-left)
    // Layout JSON already provides shift_label for number/punctuation keys.
    if (K->shift_label[0] != '\0') {
        // Centered normal label
        float scale_main = fmaxf(0.8f,(K->h*0.5f)/32.0f);
        float tw_main = text_width(K->label, scale_main);
        float tx_main = K->x + (K->w - tw_main)/2.0f;
        float ty_main = K->y + K->h*0.65f;

        // Top-left smaller shift label
        float scale_shift = scale_main * 0.7f;
        float tx_shift = K->x + 4.0f;
        float ty_shift = K->y + scale_shift * 24.0f;

        if (shift_down) {
            // When Shift is pressed: top-left small becomes white, center becomes grey
            draw_text_colored(K->label,      tx_main,  ty_main,  scale_main,  win_w, win_h, grey[0],  grey[1],  grey[2]);
            draw_text_colored(K->shift_label,tx_shift, ty_shift, scale_shift, win_w, win_h, white[0], white[1], white[2]);
        } else {
            // Default: center is white, top-left small is grey
            draw_text_colored(K->label,      tx_main,  ty_main,  scale_main,  win_w, win_h, white[0], white[1], white[2]);
            draw_text_colored(K->shift_label,tx_shift, ty_shift, scale_shift, win_w, win_h, grey[0],  grey[1],  grey[2]);
        }
        return;
    }

// 3) Special keys (words/icons) — single top-left
{
    float scale = fmaxf(0.8f,(K->h*0.5f)/32.0f);
    float tx = K->x + 4.0f;                 // padding from left
    float ty = K->y + scale * 24.0f;        // padding from top
    draw_text_colored(K->label, tx, ty, scale,
                      win_w, win_h, white[0], white[1], white[2]);
}


}


/* Load font atlas */


static void init_font(){
    unsigned char ttf_buffer[1<<20];
    unsigned char bitmap[512*512];

    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
    if (len != -1) {
        exe_path[len] = '\0';
    }
    char *dir = dirname(exe_path);

    char font_path[1064];
    snprintf(font_path, sizeof(font_path), "%s/segoeui.ttf", dir);

    FILE* f = fopen(font_path,"rb");
    if(!f){ fprintf(stderr,"Font not found at %s\n", font_path); exit(1); }

    fread(ttf_buffer,1,1<<20,f); fclose(f);
    stbtt_BakeFontBitmap(ttf_buffer,0,28.0,bitmap,512,512,32,96,cdata);
    glGenTextures(1,&fontTex);
    glBindTexture(GL_TEXTURE_2D,fontTex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_ALPHA,512,512,0,GL_ALPHA,GL_UNSIGNED_BYTE,bitmap);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
}


/*
static void init_font(){
    unsigned char ttf_buffer[1<<20];
    unsigned char bitmap[512*512];
    FILE* f=fopen("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf","rb");
    if(!f){fprintf(stderr,"Font not found\n");exit(1);}
    fread(ttf_buffer,1,1<<20,f);fclose(f);
    stbtt_BakeFontBitmap(ttf_buffer,0,24.0,bitmap,512,512,32,96,cdata);
    glGenTextures(1,&fontTex);
    glBindTexture(GL_TEXTURE_2D,fontTex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_ALPHA,512,512,0,GL_ALPHA,GL_UNSIGNED_BYTE,bitmap);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
}
*/
/* Utility for ms timestamp */
static long now_ms(){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec*1000+ts.tv_nsec/1000000;}

/* Layout loader */
typedef struct { float start,end; } Span;

typedef struct {
    char label[64];
    char action[32];
} MenuEntry;

MenuEntry pref_menu[16];
int pref_menu_count = 0;

void load_menu_json(cJSON* root) {
    cJSON* menu = cJSON_GetObjectItem(root, "menu");
    if (!menu) return;
    cJSON* prefs = cJSON_GetObjectItem(menu, "preferences");
    if (!cJSON_IsArray(prefs)) return;

    int n = cJSON_GetArraySize(prefs);
    for (int i=0; i<n && i<16; i++) {
        cJSON* item = cJSON_GetArrayItem(prefs, i);
        cJSON* lab = cJSON_GetObjectItem(item, "label");
        cJSON* act = cJSON_GetObjectItem(item, "action");
        if (cJSON_IsString(lab) && cJSON_IsString(act)) {
            strncpy(pref_menu[pref_menu_count].label, lab->valuestring, 63);
            strncpy(pref_menu[pref_menu_count].action, act->valuestring, 31);
            pref_menu_count++;
        }
    }
}


int load_layout_json(const char* path, Key* keys, int maxkeys, int win_w, int win_h){
    FILE* f=fopen(path,"rb"); if(!f){ perror("open"); return 0; }
    fseek(f,0,SEEK_END); long len=ftell(f); rewind(f);
    char* data=malloc(len+1); fread(data,1,len,f); data[len]='\0'; fclose(f);

    cJSON* root=cJSON_Parse(data);
    if(!root){ fprintf(stderr,"JSON parse error\n"); free(data); return 0; }

    load_menu_json(root);

    cJSON* rows=cJSON_GetObjectItem(root,"rows");
    if(!cJSON_IsArray(rows)){ fprintf(stderr,"no rows array\n"); cJSON_Delete(root); free(data); return 0; }

    int nrows=cJSON_GetArraySize(rows);
    float row_h=(float)win_h/nrows;

    int nkeys=0;
    Span* reserved[128]={0};
    int reserved_count[128]={0};

    const float GAP_PX    = 2.0f; // horizontal gap
    const float ROW_GAP_PX= 2.0f; // vertical gap between rows

    for(int r=0;r<nrows;r++){
        cJSON* row=cJSON_GetArrayItem(rows,r);
        if(!cJSON_IsArray(row)) continue;

        double total_units=0.0;
        int ncols=cJSON_GetArraySize(row);
        for(int c=0;c<ncols;c++){
            cJSON* obj=cJSON_GetArrayItem(row,c);
            if(!cJSON_IsObject(obj)) continue;
            cJSON* wobj=cJSON_GetObjectItem(obj,"width");
            double wmult=(cJSON_IsNumber(wobj)?wobj->valuedouble:1.0);
            if(wmult<=0.0) wmult=1.0;
            total_units+=wmult;
        }
        if(total_units<=0.0) total_units=1.0;

        double reserved_px=0.0;
        for(int s=0;s<reserved_count[r];s++){
            Span sp=reserved[r][s];
            if(sp.end>sp.start) reserved_px += (sp.end - sp.start);
        }

        int gaps_applied=(ncols>0?ncols-1:0);
        double gaps_px=(double)gaps_applied*GAP_PX;
        double effective_row_px=(double)win_w-reserved_px-gaps_px;
        if(effective_row_px<1.0) effective_row_px=1.0;

        float unit_w=(float)effective_row_px/(float)total_units;
        float xcursor=0.0f;

        for(int c=0;c<ncols;c++){
            cJSON* obj=cJSON_GetArrayItem(row,c);
            if(!cJSON_IsObject(obj)) continue;
            cJSON* lab=cJSON_GetObjectItem(obj,"label");
            cJSON* shlab=cJSON_GetObjectItem(obj,"shift_label");
            cJSON* ks=cJSON_GetObjectItem(obj,"keysym");
            if(!lab||!ks||!cJSON_IsString(lab)||!cJSON_IsString(ks)) continue;

            cJSON* wobj=cJSON_GetObjectItem(obj,"width");
            cJSON* hobj=cJSON_GetObjectItem(obj,"height");
            float wmult=(cJSON_IsNumber(wobj)?(float)wobj->valuedouble:1.0f);
            float hmult=(cJSON_IsNumber(hobj)?(float)hobj->valuedouble:1.0f);
            if(wmult<=0.0f) wmult=1.0f;
            if(hmult<=0.0f) hmult=1.0f;

            for(int s=0;s<reserved_count[r];s++){
                Span sp=reserved[r][s];
                if(xcursor>=sp.start && xcursor<sp.end){
                    xcursor=sp.end;
                }
            }

            if(nkeys<maxkeys){
                Key* K=&keys[nkeys++];
                K->label[0]='\0'; K->shift_label[0]='\0';
                strncpy(K->label,lab->valuestring,sizeof(K->label)-1);
                if(shlab&&cJSON_IsString(shlab))
                    strncpy(K->shift_label,shlab->valuestring,sizeof(K->shift_label)-1);

                // Vertical spacing applied here
                K->x=xcursor;
                K->y=r*row_h + ROW_GAP_PX*r;
                K->w=unit_w*wmult;
                K->h = row_h * hmult + ROW_GAP_PX * (hmult - 1);

                // Stretch last key to right edge, respecting reserved spans
                if(c==ncols-1){
                    float new_w=win_w-K->x;
                    for(int s=0;s<reserved_count[r];s++){
                        Span sp=reserved[r][s];
                        if(K->x<sp.end && sp.end>K->x){
                            new_w=sp.start-K->x-GAP_PX; // leave gap before reserved
                            break;
                        }
                    }
                    K->w=new_w;
                }

                xcursor=K->x+K->w;
                if(c<ncols-1) xcursor+=GAP_PX;

// Resolve keysym, with special case for Preferences
if (strcmp(ks->valuestring,"XK_Preferences")==0) {
    K->keysym = XK_Preferences;   // custom constant you defined at top of file
} else {
    const char* ks_lookup=ks->valuestring;
    if(strncmp(ks_lookup,"XK_",3)==0) ks_lookup+=3;
    K->keysym=XStringToKeysym(ks_lookup);
    if(K->keysym==NoSymbol) K->keysym=XStringToKeysym(ks->valuestring);
}


                int spans_down=(int)floorf(hmult)-1;
                for(int dr=1;dr<=spans_down;dr++){
                    int rr=r+dr; if(rr>=nrows) break;
                    int cnt=reserved_count[rr];
                    Span* arr=reserved[rr];
                    Span newspan=(Span){K->x,K->x+K->w};
                    Span* arr2=(Span*)realloc(arr,(cnt+1)*sizeof(Span));
                    if(arr2){
                        arr2[cnt]=newspan;
                        reserved[rr]=arr2;
                        reserved_count[rr]=cnt+1;
                    }
                }
            }
        }
    }

    for(int r=0;r<nrows;r++) free(reserved[r]);
    cJSON_Delete(root); free(data);
    return nkeys;
}


void draw_backspace_icon(float x, float y, float w, float h, int win_w, int win_h) {
    // Background rectangle
    float r=0.6f,g=0.6f,b=0.6f;
    RectVtx quad[6]={{x,y,r,g,b},{x+w,y,r,g,b},{x+w,y+h,r,g,b},
                     {x,y,r,g,b},{x+w,y+h,r,g,b},{x,y+h,r,g,b}};
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&quad[0].x);
    glEnableVertexAttribArray(rect_aPos);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&quad[0].r);
    glEnableVertexAttribArray(rect_aCol);
    glDrawArrays(GL_TRIANGLES,0,6);

    // Chevron: triangle pointing left
    RectVtx chevron[3]={{x+4,y+h/2,r*0.8f,g*0.8f,b*0.8f},
                        {x+16,y+4,r*0.8f,g*0.8f,b*0.8f},
                        {x+16,y+h-4,r*0.8f,g*0.8f,b*0.8f}};
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&chevron[0].x);
    glEnableVertexAttribArray(rect_aPos);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&chevron[0].r);
    glEnableVertexAttribArray(rect_aCol);
    glDrawArrays(GL_TRIANGLES,0,3);

    // Draw “×” centered inside
    float scale = fmaxf(0.6f,(h*0.6f)/32.0f);
    float tw = text_width("×", scale);
    float tx = x + (w - tw)/2.0f;
    float ty = y + h*0.6f;
    draw_text("×", tx, ty, scale, win_w, win_h);
}

void draw_menu_above_key(Key prefKey, int win_w, int win_h) {
    if (!menu_visible) return;

    float menu_w = prefKey.w;
    float menu_h = pref_menu_count * prefKey.h;
    float x = prefKey.x;
    float y = prefKey.y - menu_h - 2;   // just above the Preferences key

    if (y < 4) y = 4; // clamp so it doesn’t go off-screen

    // --- Panel background behind all menu entries ---
    float pad = 2.0f;
    float px = x - pad;
    float py = y - pad;
    float pw = menu_w + 2*pad;
    float ph = menu_h + 2*pad;
    float pr=0.15f, pg=0.15f, pb=0.18f; // darker gray panel

    RectVtx pquad[6] = {
        {px, py, pr, pg, pb},
        {px+pw, py, pr, pg, pb},
        {px+pw, py+ph, pr, pg, pb},
        {px, py, pr, pg, pb},
        {px+pw, py+ph, pr, pg, pb},
        {px, py+ph, pr, pg, pb}
    };
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&pquad[0].x);
    glEnableVertexAttribArray(rect_aPos);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&pquad[0].r);
    glEnableVertexAttribArray(rect_aCol);
    glDrawArrays(GL_TRIANGLES,0,6);

    // --- Draw each menu entry as a key ---
    for (int m=0; m<pref_menu_count; m++) {
        float ey = y + m*prefKey.h;
        float r=0.6f,g=0.6f,b=0.6f;
        if (menu_pressed == m) { r*=0.5f; g*=0.5f; b*=0.5f; } // darken if pressed

        RectVtx quad[6] = {
            {x,ey,r,g,b},{x+menu_w,ey,r,g,b},{x+menu_w,ey+prefKey.h,r,g,b},
            {x,ey,r,g,b},{x+menu_w,ey+prefKey.h,r,g,b},{x,ey+prefKey.h,r,g,b}
        };
        glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&quad[0].x);
        glEnableVertexAttribArray(rect_aPos);
        glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&quad[0].r);
        glEnableVertexAttribArray(rect_aCol);
        glDrawArrays(GL_TRIANGLES,0,6);

        // Center text inside each entry
        float scale=fmaxf(0.6f,(prefKey.h*0.6f)/32.0f);
        float tw=text_width(pref_menu[m].label,scale);
        float tx=x+(menu_w-tw)/2.0f;
        float ty=ey+prefKey.h*0.6f;
        draw_text(pref_menu[m].label, tx, ty, scale, win_w, win_h);


    }
}


void draw_menu(int win_w, int win_h) {
    if (!menu_visible) return;

    float menu_w = win_w * 0.3f;
    float menu_h = pref_menu_count * 40.0f;
    float x = (win_w - menu_w) / 2;
    float y = (win_h - menu_h) / 2;

    // Background rectangle
    RectVtx quad[6]={{x,y,0.2f,0.2f,0.2f},{x+menu_w,y,0.2f,0.2f,0.2f},
                     {x+menu_w,y+menu_h,0.2f,0.2f,0.2f},{x,y,0.2f,0.2f,0.2f},
                     {x+menu_w,y+menu_h,0.2f,0.2f,0.2f},{x,y+menu_h,0.2f,0.2f,0.2f}};
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&quad[0].x);
    glEnableVertexAttribArray(rect_aPos);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&quad[0].r);
    glEnableVertexAttribArray(rect_aCol);
    glDrawArrays(GL_TRIANGLES,0,6);

    // Entries
    for (int i=0; i<pref_menu_count; i++) {
        float ty = y + 30 + i*40;
        draw_text(pref_menu[i].label, x+20, ty, 1.0f, win_w, win_h);
    }
}



static void print_window_info(Display* dpy, Window w, const char* label) {
    if (w == None) {
        printf("%s: None\n", label);
        return;
    }
    char* name = NULL;
    Status s = XFetchName(dpy, w, &name);
    if (s && name) {
        printf("%s: 0x%lx \"%s\"\n", label, (unsigned long)w, name);
        XFree(name);
    } else {
        printf("%s: 0x%lx (no name)\n", label, (unsigned long)w);
    }
}

static void debug_window(Display *dpy, Window w) {
    XWindowAttributes attr;
    if (XGetWindowAttributes(dpy, w, &attr)) {
        printf("Window 0x%lx:\n", (unsigned long)w);
        printf("  class        = %s\n",
               (attr.class == InputOutput ? "InputOutput" :
                attr.class == InputOnly   ? "InputOnly"   : "Unknown"));
        printf("  override_red = %d\n", attr.override_redirect);
        printf("  event_mask   = 0x%lx\n", attr.your_event_mask);
        if (attr.your_event_mask & ButtonPressMask)   printf("    ButtonPressMask\n");
        if (attr.your_event_mask & ButtonReleaseMask) printf("    ButtonReleaseMask\n");
        if (attr.your_event_mask & ExposureMask)      printf("    ExposureMask\n");
    } else {
        fprintf(stderr, "XGetWindowAttributes failed\n");
    }
}



/* Deepest child under pointer (to lock onto the real input window) */
static Window deepest_under_pointer(Display *dpy, Window start) {
    Window w = start;
    for (;;) {
        Window root_ret, child;
        int rx, ry, wx, wy; unsigned int mask;
        if (!XQueryPointer(dpy, w, &root_ret, &child, &rx, &ry, &wx, &wy, &mask))
            break;
        if (child == None) break;
        w = child;
    }
    return w;
}

static Window find_input_child(Display *dpy, Window w) {
    Window root, parent, *children;
    unsigned int nchildren;
    if (XQueryTree(dpy, w, &root, &parent, &children, &nchildren)) {
        for (unsigned int i=0; i<nchildren; i++) {
            XWindowAttributes attr;
            if (XGetWindowAttributes(dpy, children[i], &attr) && attr.class == InputOutput) {
                // descend further if needed
                Window deeper = find_input_child(dpy, children[i]);
                if (deeper != None) { XFree(children); return deeper; }
                return children[i];
            }
        }
        if (children) XFree(children);
    }
    return w; // fallback
}


static int my_xerror_handler(Display *dpy, XErrorEvent *err) {
    char buf[256];
    XGetErrorText(dpy, err->error_code, buf, sizeof(buf));
    fprintf(stderr,
            "X11 error: %s (opcode %d, resource 0x%lx, serial %lu)\n",
            buf,
            err->request_code,
            err->resourceid,
            err->serial);
    // If it's BadWindow, clear last_focus
    if (err->error_code == BadWindow) {
        // reset your cached focus window
        // e.g. last_focus = None;
    }
    return 0;
}


// Draw a simple keyboard icon inside a 40x40 launcher window
static void draw_launcher_icon(int win_w, int win_h) {
    // Outer keyboard body (dark grey)
    float r_body=0.25f, g_body=0.25f, b_body=0.25f;
    RectVtx body[6] = {
        {4,4,r_body,g_body,b_body}, {36,4,r_body,g_body,b_body}, {36,36,r_body,g_body,b_body},
        {4,4,r_body,g_body,b_body}, {36,36,r_body,g_body,b_body}, {4,36,r_body,g_body,b_body}
    };
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&body[0].x);
    glEnableVertexAttribArray(rect_aPos);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&body[0].r);
    glEnableVertexAttribArray(rect_aCol);
    glDrawArrays(GL_TRIANGLES,0,6);

    // Inner keys (even darker grey)
    float r_key=0.12f, g_key=0.12f, b_key=0.12f;
    for (int row=0; row<2; row++) {
        for (int col=0; col<3; col++) {
            float kx = 8 + col*9;
            float ky = 10 + row*10;
            RectVtx key[6] = {
                {kx,ky,r_key,g_key,b_key},{kx+7,ky,r_key,g_key,b_key},{kx+7,ky+7,r_key,g_key,b_key},
                {kx,ky,r_key,g_key,b_key},{kx+7,ky+7,r_key,g_key,b_key},{kx,ky+7,r_key,g_key,b_key}
            };
            glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&key[0].x);
            glEnableVertexAttribArray(rect_aPos);
            glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&key[0].r);
            glEnableVertexAttribArray(rect_aCol);
            glDrawArrays(GL_TRIANGLES,0,6);
        }
    }

    // Optional: spacebar rectangle at bottom
    RectVtx spacebar[6] = {
        {10,26,r_key,g_key,b_key},{30,26,r_key,g_key,b_key},{30,32,r_key,g_key,b_key},
        {10,26,r_key,g_key,b_key},{30,32,r_key,g_key,b_key},{10,32,r_key,g_key,b_key}
    };
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&spacebar[0].x);
    glEnableVertexAttribArray(rect_aPos);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(RectVtx),&spacebar[0].r);
    glEnableVertexAttribArray(rect_aCol);
    glDrawArrays(GL_TRIANGLES,0,6);
}



/* ==================== MAIN ==================== */
int main(int argc,char**argv){

    Window last_focus = None;
    setbuf(stdout,NULL);
    const char* layout_path=(argc>=2)?argv[1]:"layout.json";

    Display* dpy=XOpenDisplay(NULL);
    if(!dpy){fprintf(stderr,"XOpenDisplay failed\n");return 1;}
    int screen=DefaultScreen(dpy);
    int sw=DisplayWidth(dpy,screen), sh=DisplayHeight(dpy,screen);
    int win_w=sw, win_h=sh/2.5, win_y=sh-win_h;

    XSetErrorHandler(my_xerror_handler);

XSetWindowAttributes swa;
swa.override_redirect = True;
swa.event_mask = ExposureMask;
Window win = XCreateWindow(dpy, RootWindow(dpy, screen),
                           0, win_y, win_w, win_h, 0,
                           CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWEventMask, &swa);




    XStoreName(dpy,win,"Keyboard");

    XMapWindow(dpy,win);

XRaiseWindow(dpy, win);

debug_window(dpy, win);


XSelectInput(dpy, win, ExposureMask);


// Small always-visible launcher window


XSetWindowAttributes attrs;
attrs.override_redirect = True;
//attrs.background_pixel = WhitePixel(dpy, screen);

XColor darkGrey;
Colormap cmap = DefaultColormap(dpy, screen);
XParseColor(dpy, cmap, "#303030", &darkGrey);   // dark grey
XAllocColor(dpy, cmap, &darkGrey);
attrs.background_pixel = darkGrey.pixel;


Window launcher = XCreateWindow(dpy, RootWindow(dpy, screen),
                                10, 10, 40, 40, 0,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect | CWBackPixel, &attrs);


XStoreName(dpy, launcher, "Keyboard Launcher");
XSelectInput(dpy, launcher, ButtonPressMask | ExposureMask);


//XMapWindow(dpy, launcher); // don't map until hidden


// After creating your main drawing window `win`:

// Create an InputOnly child that covers the same area
Window input = XCreateWindow(dpy, win,
                             0, 0, win_w, win_h, 0,
                             CopyFromParent, InputOnly, CopyFromParent,
                             0, NULL);

// Select for button events on the InputOnly child
XSelectInput(dpy, input, ButtonPressMask | ButtonReleaseMask);

// Map the InputOnly child so it becomes active
XMapWindow(dpy, input);

XRaiseWindow(dpy, input);
/*

Atom strut_atom = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
Atom cardinal = XInternAtom(dpy, "CARDINAL", False);

long strut[12] = {0};

// Reserve bottom `win_h` pixels
strut[3]  = win_h;     // bottom size
strut[11] = win_y;     // bottom_start_y (top of your keyboard window)
strut[10] = win_y + win_h - 1; // bottom_end_y

XChangeProperty(dpy, input, strut_atom, cardinal, 32,
                PropModeReplace, (unsigned char*)strut, 12);

*/

XSelectInput(dpy, RootWindow(dpy, screen), FocusChangeMask);

    printf("keyboard win id: 0x%lx\n", (unsigned long)win);

    EGLDisplay edpy=eglGetDisplay((EGLNativeDisplayType)dpy);
    eglInitialize(edpy,NULL,NULL);
    EGLint cfg_attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,
                        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,
                        EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_NONE};
    EGLConfig cfg; EGLint ncfg;
    eglChooseConfig(edpy,cfg_attrs,&cfg,1,&ncfg);
    EGLint ctx_attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext ctx=eglCreateContext(edpy,cfg,EGL_NO_CONTEXT,ctx_attrs);
    EGLSurface surf=eglCreateWindowSurface(edpy,cfg,(EGLNativeWindowType)win,NULL);
    EGLSurface launcher_surf = eglCreateWindowSurface(edpy, cfg,
                                    (EGLNativeWindowType)launcher, NULL);


    eglMakeCurrent(edpy,surf,surf,ctx);

    rect_prog=make_program(RECT_VS,RECT_FS);
    rect_aPos=glGetAttribLocation(rect_prog,"aPos");
    rect_aCol=glGetAttribLocation(rect_prog,"aCol");
    rect_uRes=glGetUniformLocation(rect_prog,"uRes");

    text_prog=make_program(TEXT_VS,TEXT_FS);
    text_aPos=glGetAttribLocation(text_prog,"aPos");
    text_aUV=glGetAttribLocation(text_prog,"aUV");
    text_uRes=glGetUniformLocation(text_prog,"uRes");
    text_uFont=glGetUniformLocation(text_prog,"uFont");
    text_uColor = glGetUniformLocation(text_prog,"uColor");

    glViewport(0,0,win_w,win_h);
    glClearColor(0.1f,0.1f,0.12f,1.0f);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

    init_font();

    Key keys[256];
    int nkeys=load_layout_json(layout_path,keys,256,win_w,win_h);
    printf("Loaded %d keys from %s\n",nkeys,layout_path);

    int pressed[256]={0};
    struct timespec press_time[256];
    long last_repeat[256]={0};

    bool dirty = true;

    keyboard_visible=true;

    Key get_preferences_key(Key* keys, int nkeys) {
        for (int i=0; i<nkeys; i++) if (keys[i].keysym == XK_Preferences) return keys[i];
        // Fallback (shouldn’t happen if layout has Preferences)
        Key k = {0};
        return k;
   }


    /* Capture target once at startup: use pointer location, deepest child.
       If focus already points to a valid external client, prefer that. */
    Window root = DefaultRootWindow(dpy), child;
    int rx, ry, wx, wy; unsigned int mask;
    XWindowAttributes attr;
    Window fw; int revert;
    XGetInputFocus(dpy, &fw, &revert);

    if (fw != None &&
        fw != win &&
        fw != input &&
        fw != root &&
        fw != (Window)0x1) {
        last_focus = fw;
    } else if (XQueryPointer(dpy, root, &root, &child, &rx, &ry, &wx, &wy, &mask) && child != None) {
        Window target = deepest_under_pointer(dpy, child);
        last_focus = find_input_child(dpy, target);

        if (last_focus != None && XGetWindowAttributes(dpy, last_focus, &attr)) {
            XSetInputFocus(dpy, last_focus, RevertToParent, CurrentTime);
        } else {
            last_focus = None; // reset
        }

    }

    if (last_focus != None) {
        print_window_info(dpy, last_focus, "Captured target");
        // Give it focus once, then never reassert
        if (last_focus != None && XGetWindowAttributes(dpy, last_focus, &attr)) {
            XSetInputFocus(dpy, last_focus, RevertToParent, CurrentTime);
        } else {
            last_focus = None; // reset
        }

        XSync(dpy, False);
    } else {
        fprintf(stderr, "Warning: no valid target captured; keys will be ignored.\n");
    }

    for(;;){

if (keyboard_visible) {

    Window root = DefaultRootWindow(dpy), child;
    int rx, ry, wx, wy; unsigned int mask;
    XWindowAttributes attr;
    Window fw; int revert;
    XGetInputFocus(dpy, &fw, &revert);

    if (fw != None &&
        fw != win &&
        fw != input &&
        fw != root &&
        fw != (Window)0x1) {
        last_focus = fw;
    }

    if (XQueryPointer(dpy, root, &root, &child, &rx, &ry, &wx, &wy, &mask) && child != None) {

    if (child != None &&
        child != win &&
        child != input &&
        child != root &&
        child != (Window)0x1) {

        Window target = deepest_under_pointer(dpy, child);
        last_focus = find_input_child(dpy, target);

        if (last_focus != None) {

	if (XGetWindowAttributes(dpy, last_focus, &attr)) {
            XSetInputFocus(dpy, last_focus, RevertToParent, CurrentTime);
        } else {
            last_focus = None; // reset
        }

	}
	}
    }

}

        while(XPending(dpy)){
            XEvent ev; XNextEvent(dpy,&ev);

if (ev.type == Expose && ev.xany.window == input || ev.xany.window == win) {
dirty=true;
}


if (ev.type == ButtonPress && ev.xany.window == launcher) {
    // Toggle keyboard visibility
    XWindowAttributes attr;
    if (XGetWindowAttributes(dpy, win, &attr)) {
        if (attr.map_state == IsViewable) {


            XUnmapWindow(dpy, win);   // hide
            keyboard_visible = false;
	    eglMakeCurrent(edpy, launcher_surf, launcher_surf, ctx);
	    glViewport(0,0,40,40);
	    glClearColor(0.25f,0.25f,0.25f,1.0f); // dark grey background
	    glClear(GL_COLOR_BUFFER_BIT);

// Draw text centered
float scale = 1.0f;
float tw = text_width("⌨", scale);
float tx = (40 - tw)/2.0f;
float ty = 25; // vertical center
draw_text_colored("⌨", tx, ty, scale, 40, 40, 0.12f,0.12f,0.12f); // dark dark grey

eglSwapBuffers(edpy, launcher_surf);



        } else {
	    eglMakeCurrent(edpy, surf, surf, ctx);
            XMapWindow(dpy, win);     // show
            XUnmapWindow(dpy, launcher);
            keyboard_visible = true;
        }
    }

    // Reset Preferences key pressed state
    for (int i=0; i<nkeys; i++) {
        if (keys[i].keysym == XK_Preferences) {
            pressed[i] = 0;
            break;
        }
    }

    menu_visible = false; // close menu if it was open

    dirty = true;
    continue;
}



            if (ev.type == ButtonPress && ev.xany.window == input){


// --- Handle menu clicks first ---
if (menu_visible) {
    Key prefKey = get_preferences_key(keys, nkeys);

    float menu_w = prefKey.w;
    float menu_h = pref_menu_count * prefKey.h;
    float x = prefKey.x;
    float y = prefKey.y - menu_h - 2;

    // Click inside menu: press that entry (darken)
    if (ev.xbutton.x >= x && ev.xbutton.x < x+menu_w &&
        ev.xbutton.y >= y && ev.xbutton.y < y+menu_h) {
        int idx = (ev.xbutton.y - y) / prefKey.h;
        if (idx >= 0 && idx < pref_menu_count) {
            menu_pressed = idx;
            dirty = true;
        }
        // Consume event so keys behind don’t get it
        continue;
    }

    // Optional: click outside menu closes it (and consume)
    menu_visible = false;
    menu_pressed = -1;
    dirty = true;
    continue;
}


                for (int i = 0; i < nkeys; i++) {
                    if (ev.xbutton.x >= keys[i].x && ev.xbutton.x < keys[i].x + keys[i].w &&
                        ev.xbutton.y >= keys[i].y && ev.xbutton.y < keys[i].y + keys[i].h) {
                        pressed[i] = 1;
                        clock_gettime(CLOCK_MONOTONIC, &press_time[i]);
                        last_repeat[i] = 0;

		        dirty = true;


            // --- Preferences key toggle ---
            if (keys[i].keysym == XK_Preferences) {
                menu_visible = !menu_visible;
                dirty = true;
                goto handled_press;
            }


                        // --- Modifier toggles ---
                        if (keys[i].keysym == XK_Shift_L || keys[i].keysym == XK_Shift_R) {
                            shift_down = !shift_down;
                            pressed[i] = shift_down;
                dirty = true;
                            goto handled_press;
                        }
                        if (keys[i].keysym == XK_Caps_Lock) {
                            caps_down = !caps_down;
                            pressed[i] = 0;
                dirty = true;
                            goto handled_press;
                        }
                        if (keys[i].keysym == XK_Control_L || keys[i].keysym == XK_Control_R) {
                            ctrl_down = !ctrl_down;
                            pressed[i] = ctrl_down;
                dirty = true;
                            goto handled_press;
                        }
                        if (keys[i].keysym == XK_Alt_L || keys[i].keysym == XK_Alt_R) {
                            alt_down = !alt_down;
                            pressed[i] = alt_down;
                dirty = true;
                            goto handled_press;
                        }

			if (keys[i].keysym == XK_Mode_switch) {  // Fn key
			    fn_down = !fn_down;          // activate Fn
			    pressed[i] = 1;       // show it pressed
                dirty = true;
			    goto handled_press;
			}


                            KeySym base = keys[i].keysym;


    // --- Fn remapping: if fn_down is active, remap number row to F1–F12 ---
    if (fn_down) {
        switch (base) {
            case XK_1:     base = XK_F1;  break;
            case XK_2:     base = XK_F2;  break;
            case XK_3:     base = XK_F3;  break;
            case XK_4:     base = XK_F4;  break;
            case XK_5:     base = XK_F5;  break;
            case XK_6:     base = XK_F6;  break;
            case XK_7:     base = XK_F7;  break;
            case XK_8:     base = XK_F8;  break;
            case XK_9:     base = XK_F9;  break;
            case XK_0:     base = XK_F10; break;
            case XK_minus: base = XK_F11; break;
            case XK_equal: base = XK_F12; break;
        }
        fn_down = 0;   // auto-release after one use

        // clear Fn pressed state so the button visually resets
//        for (int j=0; j<nkeys; j++) {
//            if (keys[j].keysym == XK_Mode_switch) {
//                pressed[j] = 0;
//                break;
//           }
	    dirty = true;

//        }
    }
                        // --- Normal key injection (no focus change) ---
                        if (last_focus != None) {

                            KeyCode kc  = XKeysymToKeycode(dpy, base);
                            KeyCode skc = XKeysymToKeycode(dpy, XK_Shift_L);
                            KeyCode ckc = XKeysymToKeycode(dpy, XK_Control_L);
                            KeyCode akc = XKeysymToKeycode(dpy, XK_Alt_L);

                            int need_shift = 0;
                            if (strlen(keys[i].label) == 1 && isalpha((unsigned char)keys[i].label[0])) {
                                if (caps_down ^ shift_down) need_shift = 1;
                            } else {
                                if (shift_down) need_shift = 1;
                            }

                            // Press modifiers if needed
                            if (need_shift && skc) XTestFakeKeyEvent(dpy, skc, True, 0);
                            if (ctrl_down && ckc)  XTestFakeKeyEvent(dpy, ckc, True, 0);
                            if (alt_down && akc)   XTestFakeKeyEvent(dpy, akc, True, 0);

                            // Actual key
                            if (kc) {
                                XTestFakeKeyEvent(dpy, kc, True, 0);
                                XTestFakeKeyEvent(dpy, kc, False, 0);
                            }

                            // Release modifiers if auto-release
                            if (alt_down && akc)   XTestFakeKeyEvent(dpy, akc, False, 0);
                            if (ctrl_down && ckc)  XTestFakeKeyEvent(dpy, ckc, False, 0);
                            if (need_shift && skc) XTestFakeKeyEvent(dpy, skc, False, 0);

                            XFlush(dpy);
                        }

                        // --- Reset modifiers after non-modifier key ---
                        if (keys[i].keysym != XK_Shift_L && keys[i].keysym != XK_Shift_R &&
                            keys[i].keysym != XK_Control_L && keys[i].keysym != XK_Control_R &&
                            keys[i].keysym != XK_Alt_L && keys[i].keysym != XK_Alt_R &&
                            keys[i].keysym != XK_Caps_Lock) {
                            shift_down = 0; ctrl_down = 0; alt_down = 0;
                            for (int j = 0; j < nkeys; j++) {
                                if (keys[j].keysym == XK_Shift_L || keys[j].keysym == XK_Shift_R) pressed[j] = 0;
                                if (keys[j].keysym == XK_Control_L || keys[j].keysym == XK_Control_R) pressed[j] = 0;
                                if (keys[j].keysym == XK_Alt_L || keys[j].keysym == XK_Alt_R) pressed[j] = 0;
                            }
                        }

                        goto handled_press;
                    }
                }
                handled_press: ;
            }

            else if(ev.type==ButtonRelease){


if (menu_visible) {
    Key prefKey = get_preferences_key(keys, nkeys);

    float menu_w = prefKey.w;
    float menu_h = pref_menu_count * prefKey.h;
    float x = prefKey.x;
    float y = prefKey.y - menu_h - 2;

    if (menu_pressed != -1) {
        int idx = menu_pressed;
        menu_pressed = -1;

        // Only trigger if release is still inside the pressed entry (like a key)
        if (ev.xbutton.x >= x && ev.xbutton.x < x+menu_w &&
            ev.xbutton.y >= y + idx*prefKey.h && ev.xbutton.y < y + (idx+1)*prefKey.h) {
            if (strcmp(pref_menu[idx].action,"quit")==0) {
                exit(0);
            } 
else if (strcmp(pref_menu[idx].action,"hide")==0) {
    // Release all pressed keys
    for (int i=0; i<nkeys; i++) {
        if (pressed[i]) {
            KeyCode kc = XKeysymToKeycode(dpy, keys[i].keysym);
            if (kc) XTestFakeKeyEvent(dpy, kc, False, 0);
            pressed[i] = 0;
        }
    }
    XFlush(dpy);

    // Hide keyboard, show launcher
    XUnmapWindow(dpy, win);
    XMapWindow(dpy, launcher);
    keyboard_visible = false;
    menu_visible = false;
}

        }
    }

    dirty = true;
    // Consume event — do not let keys behind handle it
    continue;
}



                for(int i=0;i<nkeys;i++){
                    if(pressed[i]){
                        if(keys[i].keysym==XK_Caps_Lock){
                            pressed[i]=0;
                        }
                        else if(keys[i].keysym==XK_Shift_L||keys[i].keysym==XK_Shift_R){
                            pressed[i]=shift_down;
                        }
                        else if(keys[i].keysym==XK_Control_L||keys[i].keysym==XK_Control_R){
                            pressed[i]=ctrl_down;
                        }
                        else if(keys[i].keysym==XK_Alt_L||keys[i].keysym==XK_Alt_R){
                            pressed[i]=alt_down;
                        }
                        else pressed[i]=0;
                        last_repeat[i]=0;

			dirty = true;

                        break;
                    }
                }

            }
        }

        long now = now_ms();
        for (int i = 0; i < nkeys; i++) {
            if (pressed[i]) {
                long t0 = press_time[i].tv_sec*1000 + press_time[i].tv_nsec/1000000;
                long dt = now - t0;

                // Start repeating after 400ms, then every 100ms
                if (dt > 400 && (last_repeat[i] == 0 || now - last_repeat[i] > 100)) {
                    if (last_focus != None) {
                        KeySym base = keys[i].keysym;
                        KeyCode kc  = XKeysymToKeycode(dpy, base);
                        KeyCode skc = XKeysymToKeycode(dpy, XK_Shift_L);

                        int need_shift = 0;
                        if (strlen(keys[i].label) == 1 && isalpha((unsigned char)keys[i].label[0])) {
                            if (caps_down ^ shift_down) need_shift = 1;
                        } else {
                            if (shift_down) need_shift = 1;
                        }

                        // Press modifiers if needed
                        if (need_shift && skc) XTestFakeKeyEvent(dpy, skc, True, 0);

                        // Actual key
                        if (kc) {
                            XTestFakeKeyEvent(dpy, kc, True, 0);
                            XTestFakeKeyEvent(dpy, kc, False, 0);
                        }

                        // Release modifiers
                        if (need_shift && skc) XTestFakeKeyEvent(dpy, skc, False, 0);

                        XFlush(dpy);
                    }
                    last_repeat[i] = now;

		    dirty = true;

                }
            }
        }

if (dirty) {
    glClear(GL_COLOR_BUFFER_BIT);

draw_keys(win_w, win_h, keys, nkeys, pressed, caps_down);

for (int i=0; i<nkeys; i++) {
    draw_key_labels(&keys[i], win_w, win_h, shift_down, caps_down);
}


/*
        float scale=fmaxf(0.6f,(keys[i].h*0.6f)/32.0f);
        float w=text_width(s,scale);
        float x=keys[i].x+(keys[i].w-w)/2.0f;
        float y=keys[i].y+keys[i].h*0.6f;
        draw_text(s,x,y,scale,win_w,win_h);
*/


for (int i = 0; i < nkeys; i++) {
    // Special-case: draw Windows-style backspace icon

// UP chevron
if (keys[i].keysym == XK_Up) {
    float x = keys[i].x, y = keys[i].y;
    float cx = x + 18.0f;   // top-left anchor
    float cy = y + 10.0f;

    float wsize = 13.0f;   // wider horizontal half-span
    float hsize = 5.0f;    // shallow vertical height
    float s = 2.0f;        // stroke thickness

    float r=1,g=1,b=1; if (pressed[i]) r=g=b=0.75f;
    typedef struct { float x,y,r,g,b; } V;

float tipX = cx;
float tipY = cy - hsize;

    glUseProgram(rect_prog);
    glUniform2f(rect_uRes,(float)win_w,(float)win_h);

    // Left arm: bottom-left to tip
V left[6] = {
    {cx-wsize, cy+hsize, r,g,b},
    {cx-wsize+s, cy+hsize, r,g,b},
    {tipX, tipY, r,g,b},
    {cx-wsize, cy+hsize, r,g,b},
    {tipX, tipY, r,g,b},
    {tipX-s, tipY, r,g,b}   // overlap at tip
};

    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&left[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&left[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

V right[6] = {
    {cx+wsize, cy+hsize, r,g,b},
    {cx+wsize-s, cy+hsize, r,g,b},
    {tipX, tipY, r,g,b},
    {cx+wsize, cy+hsize, r,g,b},
    {tipX, tipY, r,g,b},
    {tipX+s, tipY, r,g,b}   // overlap at tip
};
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&right[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&right[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

    continue;
}

if (keys[i].keysym == XK_Down) {
    float x = keys[i].x, y = keys[i].y;
    float cx = x + 18.0f;   // top-left anchor
    float cy = y + 16.0f;

    float wsize = 13.0f;
    float hsize = 5.0f;
    float s = 2.0f;

    float r=1,g=1,b=1; if (pressed[i]) r=g=b=0.75f;
    typedef struct { float x,y,r,g,b; } V;

float tipX = cx;
float tipY = cy + hsize;

    glUseProgram(rect_prog);
    glUniform2f(rect_uRes,(float)win_w,(float)win_h);

    // Left arm: top-left to tip

V left[6] = {
    {cx-wsize, cy-hsize, r,g,b},
    {cx-wsize+s, cy-hsize, r,g,b},
    {tipX, tipY, r,g,b},
    {cx-wsize, cy-hsize, r,g,b},
    {tipX, tipY, r,g,b},
    {tipX-s, tipY, r,g,b}
};

    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&left[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&left[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

    // Right arm: top-right to tip

V right[6] = {
    {cx+wsize, cy-hsize, r,g,b},
    {cx+wsize-s, cy-hsize, r,g,b},
    {tipX, tipY, r,g,b},
    {cx+wsize, cy-hsize, r,g,b},
    {tipX, tipY, r,g,b},
    {tipX+s, tipY, r,g,b}
};

    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&right[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&right[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

    continue;
}

if (keys[i].keysym == XK_Left) {
    float x = keys[i].x, y = keys[i].y;
    float cx = x + 18.0f;
    float cy = y + 12.0f;

    float wsize = 2.0f;   // shallow horizontal
    float hsize = 10.0f;  // tall vertical span
    float s = 2.0f;

    float r=1,g=1,b=1; if (pressed[i]) r=g=b=0.75f;
    typedef struct { float x,y,r,g,b; } V;

    glUseProgram(rect_prog);
    glUniform2f(rect_uRes,(float)win_w,(float)win_h);

    // Upper arm: top-right to tip
    V upper[6] = {
        {cx+wsize, cy-hsize, r,g,b},
        {cx+wsize, cy-hsize+s, r,g,b},
        {cx-hsize, cy, r,g,b},
        {cx+wsize, cy-hsize, r,g,b},
        {cx-hsize, cy, r,g,b},
        {cx-hsize, cy-s, r,g,b}
    };
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&upper[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&upper[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

    // Lower arm: bottom-right to tip
    V lower[6] = {
        {cx+wsize, cy+hsize, r,g,b},
        {cx+wsize, cy+hsize-s, r,g,b},
        {cx-hsize, cy, r,g,b},
        {cx+wsize, cy+hsize, r,g,b},
        {cx-hsize, cy, r,g,b},
        {cx-hsize, cy+s, r,g,b}
    };
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&lower[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&lower[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

    continue;
}

if (keys[i].keysym == XK_Right) {
    float x = keys[i].x, y = keys[i].y;
    float cx = x + 12.0f;
    float cy = y + 12.0f;

    float wsize = 2.0f;
    float hsize = 10.0f;
    float s = 2.0f;

    float r=1,g=1,b=1; if (pressed[i]) r=g=b=0.75f;
    typedef struct { float x,y,r,g,b; } V;

    glUseProgram(rect_prog);
    glUniform2f(rect_uRes,(float)win_w,(float)win_h);

    // Upper arm: top-left to tip
    V upper[6] = {
        {cx-wsize, cy-hsize, r,g,b},
        {cx-wsize, cy-hsize+s, r,g,b},
        {cx+hsize, cy, r,g,b},
        {cx-wsize, cy-hsize, r,g,b},
        {cx+hsize, cy, r,g,b},
        {cx+hsize, cy-s, r,g,b}
    };
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&upper[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&upper[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

    // Lower arm: bottom-left to tip
    V lower[6] = {
        {cx-wsize, cy+hsize, r,g,b},
        {cx-wsize, cy+hsize-s, r,g,b},
        {cx+hsize, cy, r,g,b},
        {cx-wsize, cy+hsize, r,g,b},
        {cx+hsize, cy, r,g,b},
        {cx+hsize, cy+s, r,g,b}
    };
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&lower[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&lower[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

    continue;
}



if (keys[i].keysym == XK_Preferences) {

    float x = keys[i].x, y = keys[i].y, w = keys[i].w, h = keys[i].h;

    // Position top-left with padding
    float cx = x + 14.0f;
    float cy = y + 14.0f;
    float outer_r = h * 0.18f;   // outer radius
    float inner_r = h * 0.10f;   // inner hole radius
    int teeth = 8;
    float stroke = 2.0f;         // outline thickness

    float r=0.0f,g=0.0f,b=0.0f;  // black outline
    if (pressed[i]) { r=g=b=0.3f; } // dark grey when pressed

    typedef struct { float x,y,r,g,b; } V;
    glUseProgram(rect_prog);
    glUniform2f(rect_uRes,(float)win_w,(float)win_h);

    // Teeth outlines
    for(int t=0;t<teeth;t++){
        float a = 2*M_PI*t/teeth;
        float tx0 = cx+(outer_r-stroke)*cosf(a);
        float ty0 = cy+(outer_r-stroke)*sinf(a);
        float tx1 = cx+(outer_r+stroke*2)*cosf(a);
        float ty1 = cy+(outer_r+stroke*2)*sinf(a);
        float w2 = stroke;
        V tooth[6] = {
            {tx0-w2*sinf(a),ty0+w2*cosf(a),r,g,b},
            {tx1-w2*sinf(a),ty1+w2*cosf(a),r,g,b},
            {tx1+w2*sinf(a),ty1-w2*cosf(a),r,g,b},
            {tx0-w2*sinf(a),ty0+w2*cosf(a),r,g,b},
            {tx1+w2*sinf(a),ty1-w2*cosf(a),r,g,b},
            {tx0+w2*sinf(a),ty0-w2*cosf(a),r,g,b}
        };
        glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&tooth[0].x);
        glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&tooth[0].r);
        glDrawArrays(GL_TRIANGLES,0,6);
    }

    // Outer ring outline (approximate circle with quads)
    int n=24;
    for(int k=0;k<n;k++){
        float a0 = 2*M_PI*k/n;
        float a1 = 2*M_PI*(k+1)/n;
        V ring[6] = {
            {cx+(outer_r-stroke)*cosf(a0), cy+(outer_r-stroke)*sinf(a0), r,g,b},
            {cx+(outer_r+stroke)*cosf(a0), cy+(outer_r+stroke)*sinf(a0), r,g,b},
            {cx+(outer_r+stroke)*cosf(a1), cy+(outer_r+stroke)*sinf(a1), r,g,b},
            {cx+(outer_r-stroke)*cosf(a0), cy+(outer_r-stroke)*sinf(a0), r,g,b},
            {cx+(outer_r+stroke)*cosf(a1), cy+(outer_r+stroke)*sinf(a1), r,g,b},
            {cx+(outer_r-stroke)*cosf(a1), cy+(outer_r-stroke)*sinf(a1), r,g,b}
        };
        glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&ring[0].x);
        glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&ring[0].r);
        glDrawArrays(GL_TRIANGLES,0,6);
    }

    // Inner hole (draw in key background color to punch it out)
    int m=24;
    for(int k=0;k<m;k++){
        float a0 = 2*M_PI*k/m;
        float a1 = 2*M_PI*(k+1)/m;
        V hole[3] = {
            {cx,cy, 0.2f,0.2f,0.2f}, // background color
            {cx+inner_r*cosf(a0), cy+inner_r*sinf(a0), 0.2f,0.2f,0.2f},
            {cx+inner_r*cosf(a1), cy+inner_r*sinf(a1), 0.2f,0.2f,0.2f}
        };
        glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&hole[0].x);
        glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&hole[0].r);
        glDrawArrays(GL_TRIANGLES,0,3);
    }

    continue;


}


if (keys[i].keysym == XK_BackSpace) {
    float x = keys[i].x, y = keys[i].y, w = keys[i].w, h = keys[i].h;

    // Fixed scale for X
    float scale = fmaxf(0.6f,(h*0.5f)/32.0f);
    float tw    = text_width("X", scale);
    float th    = scale * 20.0f; // approx glyph height

    // Box size:
float pad   = 1.0f;
float box_w = tw + pad*2;
float box_h = th + pad*2 - 2.0f; // 2px shorter vertically

    // Position top-left inside key
    float cx = x + 10.0f;
    float cy = y + 4.0f;

    float s = 1.0f; // stroke thickness
    float r=1.0f,g=1.0f,b=1.0f;
    if (pressed[i]) { r = g = b = 0.75f; }

    typedef struct { float x,y,r,g,b; } V;
    glUseProgram(rect_prog);
    glUniform2f(rect_uRes, (float)win_w, (float)win_h);

    // Top stroke
    V top[6] = {
        {cx,cy, r,g,b},{cx+box_w,cy, r,g,b},{cx+box_w,cy+s, r,g,b},
        {cx,cy, r,g,b},{cx+box_w,cy+s, r,g,b},{cx,cy+s, r,g,b}
    };
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&top[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&top[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

    // Bottom stroke
    V bottom[6] = {
        {cx,cy+box_h-s, r,g,b},{cx+box_w,cy+box_h-s, r,g,b},{cx+box_w,cy+box_h, r,g,b},
        {cx,cy+box_h-s, r,g,b},{cx+box_w,cy+box_h, r,g,b},{cx,cy+box_h, r,g,b}
    };
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&bottom[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&bottom[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

    // Right stroke
    V right[6] = {
        {cx+box_w-s,cy, r,g,b},{cx+box_w,cy, r,g,b},{cx+box_w,cy+box_h, r,g,b},
        {cx+box_w-s,cy, r,g,b},{cx+box_w,cy+box_h, r,g,b},{cx+box_w-s,cy+box_h, r,g,b}
    };
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&right[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&right[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

    // Left chevron strokes — tip further left for steeper angle
    float tip_x = cx - box_w * 0.6f;   // move tip further left
    float tip_y = cy + box_h * 0.5f;

    V chevr_up[6] = {
        {tip_x,tip_y, r,g,b},{cx,cy, r,g,b},{cx,cy+s, r,g,b},
        {tip_x,tip_y, r,g,b},{cx,cy+s, r,g,b},{tip_x+s,tip_y, r,g,b}
    };
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&chevr_up[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&chevr_up[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

    V chevr_dn[6] = {
        {tip_x,tip_y, r,g,b},{cx,cy+box_h-s, r,g,b},{cx,cy+box_h, r,g,b},
        {tip_x,tip_y, r,g,b},{cx,cy+box_h, r,g,b},{tip_x+s,tip_y, r,g,b}
    };
    glVertexAttribPointer(rect_aPos,2,GL_FLOAT,GL_FALSE,sizeof(V),&chevr_dn[0].x);
    glVertexAttribPointer(rect_aCol,3,GL_FLOAT,GL_FALSE,sizeof(V),&chevr_dn[0].r);
    glDrawArrays(GL_TRIANGLES,0,6);

    // Draw "X" inside — lower baseline slightly
    float tx = cx + pad - 0.01f;
    float ty = cy + box_h*0.76f; // lowered baseline
    draw_text("x", tx, ty, scale, win_w, win_h);

    continue;
}

/*
if (keys[i].keysym == XK_Mode_switch) {  // Fn key
    fn_down = 1;          // activate Fn
    pressed[i] = 1;       // show it pressed
    goto handled_press;
}
*/

    // Compute the label string with your existing logic
    const char* s = keys[i].label;
    static char buf[64];
    if (strlen(keys[i].label) == 1 && isalpha((unsigned char)keys[i].label[0])) {
        strcpy(buf, keys[i].label);
        buf[0] = (caps_down ^ shift_down) ? toupper(buf[0]) : tolower(buf[0]);
        s = buf;
    } else if (shift_down && keys[i].shift_label[0]) {
        s = keys[i].shift_label;
    }

/*
// Top-left aligned text
float pad_x = 6.0f;   // left padding
float pad_y = 20.0f;  // baseline offset from top
float scale = fmaxf(0.6f,(keys[i].h*0.6f)/32.0f);

float tx = keys[i].x + pad_x;
float ty = keys[i].y + pad_y;

//    glUseProgram(text_prog);

draw_text(s, tx, ty, scale, win_w, win_h);
*/

}




    // --- Draw popup menu above Preferences key ---

// --- Draw popup menu above Preferences key ---
if (menu_visible) {
    for (int i=0; i<nkeys; i++) {
        if (keys[i].keysym == XK_Preferences) {
            draw_menu_above_key(keys[i], win_w, win_h);
            break;
        }
    }
}


    eglSwapBuffers(edpy,surf);
    dirty = false;
}


        if(!XPending(dpy)) usleep(20000);
    }
}
