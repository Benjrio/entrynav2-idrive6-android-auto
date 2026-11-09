#pragma once
#include "frame_bridge.h"
static GLuint aa_texture, aa_program, aa_buffer;
static unsigned aa_width, aa_height, aa_epoch, aa_has_frame;
static GLuint aa_shader(GLenum kind,const char *source) {
    GLuint s=glCreateShader(kind); glShaderSource(s,1,&source,NULL); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){glDeleteShader(s);return 0;} return s;
}
static int aa_canvas_init(void) {
    GLuint vs=aa_shader(GL_VERTEX_SHADER,"attribute vec2 p; varying vec2 uv; void main(){gl_Position=vec4(p,0.,1.);uv=vec2((p.x+1.)*.5,(1.-p.y)*.5);}");
    GLuint fs=aa_shader(GL_FRAGMENT_SHADER,"precision mediump float; varying vec2 uv; uniform sampler2D image; void main(){gl_FragColor=texture2D(image,uv);}");
    if(!vs||!fs){if(vs)glDeleteShader(vs);if(fs)glDeleteShader(fs);return 0;}
    aa_program=glCreateProgram();glAttachShader(aa_program,vs);glAttachShader(aa_program,fs);
    glBindAttribLocation(aa_program,0,"p");glLinkProgram(aa_program);glDeleteShader(vs);glDeleteShader(fs);
    GLint ok=0;glGetProgramiv(aa_program,GL_LINK_STATUS,&ok);if(!ok)return 0;
    glUseProgram(aa_program);glUniform1i(glGetUniformLocation(aa_program,"image"),0);
    GLfloat vertices[]={-1,-1,1,-1,-1,1,1,1};
    glGenBuffers(1,&aa_buffer);glBindBuffer(GL_ARRAY_BUFFER,aa_buffer);
    glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STATIC_DRAW);
    glGenTextures(1,&aa_texture);glBindTexture(GL_TEXTURE_2D,aa_texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);return glGetError()==GL_NO_ERROR;
}
/* Returns 1 when a newly published image or invalidation needs a swap. */
static int aa_canvas_receive(struct ow_aa_frame *f) {
    unsigned epoch=atomic_load(&f->epoch), changed=0;
    if(aa_has_frame&&(!atomic_load(&f->visible)||aa_epoch!=epoch)){aa_has_frame=0;changed=1;}
    unsigned expected=2;
    if(!atomic_compare_exchange_strong_explicit(&f->slot,&expected,3,memory_order_acquire,memory_order_relaxed))return changed;
    if(f->width&&f->height&&f->width<=OW_AA_MAX_WIDTH&&f->height<=OW_AA_MAX_HEIGHT&&
       f->frame_epoch==epoch&&atomic_load(&f->visible)){
        aa_width=f->width;aa_height=f->height;aa_epoch=epoch;
        glBindTexture(GL_TEXTURE_2D,aa_texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,aa_width,aa_height,0,GL_RGB,GL_UNSIGNED_BYTE,f->rgb);
        aa_has_frame=1;changed=1;
    }
    atomic_store_explicit(&f->slot,0,memory_order_release);return changed;
}
static void aa_canvas_paint(void) {
    glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    if(!aa_has_frame)return;
    unsigned w=OW_WIDTH,h=(uint64_t)w*aa_height/aa_width;
    if(h>OW_HEIGHT){h=OW_HEIGHT;w=(uint64_t)h*aa_width/aa_height;}
    glViewport((OW_WIDTH-w)/2,(OW_HEIGHT-h)/2,w,h);glUseProgram(aa_program);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,aa_texture);glBindBuffer(GL_ARRAY_BUFFER,aa_buffer);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,NULL);glEnableVertexAttribArray(0);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}
static void aa_canvas_close(void) {
    if(aa_texture)glDeleteTextures(1,&aa_texture);if(aa_buffer)glDeleteBuffers(1,&aa_buffer);if(aa_program)glDeleteProgram(aa_program);
}
