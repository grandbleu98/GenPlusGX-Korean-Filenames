/*****************************************************************************
 * font.c
 *
 *   IPL font engine (using GX rendering)
 *
 *  Copyright Eke-Eke (2009-2016)
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include the
 *     complete source code, including the source code for all components used by a
 *     binary built from the modified sources. However, as a special exception, the
 *     source code distributed need not include anything that is normally distributed
 *     (in either source or binary form) with the major components (compiler, kernel,
 *     and so on) of the operating system on which the executable runs, unless that
 *     component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************************/

#include "shared.h"
#include "font.h"

#define _SHIFTR(v, s, w)	\
    ((u32)(((u32)(v) >> (s)) & ((0x01 << (w)) - 1)))

typedef struct _yay0header {
	unsigned int id ATTRIBUTE_PACKED;
	unsigned int dec_size ATTRIBUTE_PACKED;
	unsigned int links_offset ATTRIBUTE_PACKED;
	unsigned int chunks_offset ATTRIBUTE_PACKED;
} yay0header;

static u8 *fontImage;
static u8 *fontTexture;
static GXTexObj fontTexObj;
static void *ipl_fontarea;
static sys_fontheader *fontHeader;
static u8 font_size[256];
static u8 *hangulTexture;
static GXTexObj hangulTexObj;
#include "font_korean.h"

#ifndef HW_RVL

/* disable Qoob Modchip before IPL access (emukiddid) */
static void ipl_set_config(unsigned char c)
{
  volatile unsigned long* exi = (volatile unsigned long*)0xCC006800;
  unsigned long val,addr;
  addr=0xc0000000;
  val = c << 24;
  exi[0] = ((((exi[0]) & 0x405) | 256) | 48);     //select IPL
  //write addr of IPL
  exi[0 * 5 + 4] = addr;
  exi[0 * 5 + 3] = ((4 - 1) << 4) | (1 << 2) | 1;
  while (exi[0 * 5 + 3] & 1);
  //write the ipl we want to send
  exi[0 * 5 + 4] = val;
  exi[0 * 5 + 3] = ((4 - 1) << 4) | (1 << 2) | 1;
  while (exi[0 * 5 + 3] & 1);
  exi[0] &= 0x405;        //deselect IPL
}

#endif

static void decode_szp(void *src,void *dest)
{
	u32 i,k,link;
	u8 *dest8,*tmp;
	u32 loff,coff,roff;
	u32 size,cnt,cmask,bcnt;
	yay0header *header;

	dest8 = (u8*)dest;
	header = (yay0header*)src;
	size = header->dec_size;
	loff = header->links_offset;
	coff = header->chunks_offset;

	roff = sizeof(yay0header);
	cmask = 0;
	cnt = 0;
	bcnt = 0;

	do {
		if(!bcnt) {
			cmask = *(u32*)(src+roff);
			roff += 4;
			bcnt = 32;
		}

		if(cmask&0x80000000) {
			dest8[cnt++] = *(u8*)(src+coff);
			coff++;
		} else {
			link = *(u16*)(src+loff);
			loff += 2;

			tmp = dest8+(cnt-(link&0x0fff)-1);
			k = link>>12;
			if(k==0) {
				k = (*(u8*)(src+coff))+18;
				coff++;
			} else k += 2;

			for(i=0;i<k;i++) {
				dest8[cnt++] = tmp[i];
			}
		}
		cmask <<= 1;
		bcnt--;
	} while(cnt<size);
}

static void expand_font(u8 *src,u8 *dest)
{
	s32 cnt;
	u32 idx;
	u8 val1,val2;
  sys_fontheader *sys_fontdata = fontHeader;
	u8 *data = (u8*)sys_fontdata+44;

	if(sys_fontdata->sheet_format==0x0000) {
		cnt = (sys_fontdata->sheet_fullsize/2)-1;

		while(cnt>=0) {
			idx = _SHIFTR(src[cnt],6,2);
			val1 = data[idx];

			idx = _SHIFTR(src[cnt],4,2);
			val2 = data[idx];

			dest[(cnt<<1)+0] =((val1&0xf0)|(val2&0x0f));

			idx = _SHIFTR(src[cnt],2,2);
			val1 = data[idx];

			idx = _SHIFTR(src[cnt],0,2);
			val2 = data[idx];

			dest[(cnt<<1)+1] =((val1&0xf0)|(val2&0x0f));

			cnt--;
		}
	}
	DCStoreRange(dest,sys_fontdata->sheet_fullsize);
}

static void GetFontTexel(s32 c,void *image,s32 pos,s32 stride)
{
	u32 sheets,rem;
	u32 xoff,yoff;
	u32 xpos,ypos;
	u8 *img_start;
	u8 *ptr1,*ptr2;
  sys_fontheader *sys_fontdata = fontHeader;

	if(c<sys_fontdata->first_char || c>sys_fontdata->last_char) c = sys_fontdata->inval_char;
	else c -= sys_fontdata->first_char;

	sheets = sys_fontdata->sheet_column*sys_fontdata->sheet_row;
	rem = c%sheets;
  sheets = c/sheets;
	xoff = (rem%sys_fontdata->sheet_column)*sys_fontdata->cell_width;
	yoff = (rem/sys_fontdata->sheet_column)*sys_fontdata->cell_height;
	img_start = fontImage+(sys_fontdata->sheet_size*sheets);

	ypos = 0;
	while(ypos<sys_fontdata->cell_height) {
		xpos = 0;
		while(xpos<sys_fontdata->cell_width) {
			ptr1 = img_start+(((sys_fontdata->sheet_width/8)<<5)*((ypos+yoff)/8));
			ptr1 = ptr1+(((xpos+xoff)/8)<<5);
			ptr1 = ptr1+(((ypos+yoff)%8)<<2);
			ptr1 = ptr1+(((xpos+xoff)%8)/2);

			ptr2 = image+((ypos/8)*(((stride<<1)/8)<<5));
			ptr2 = ptr2+(((xpos+pos)/8)<<5);
			ptr2 = ptr2+(((xpos+pos)%8)/2);
			ptr2 = ptr2+((ypos%8)<<2);

			*ptr2 = *ptr1;

			xpos += 2;
		}
		ypos++;
	}
}

static void DrawChar(unsigned char c, int xpos, int ypos, int size, GXColor color)
{
  /* get font texture data */
  memset(fontTexture,0,fontHeader->cell_width * fontHeader->cell_height / 2);
  GetFontTexel(c,fontTexture,0,fontHeader->cell_width/2);
  DCStoreRange(fontTexture, fontHeader->cell_width * fontHeader->cell_height / 2);
  GX_InvalidateTexAll();

  /* adjust texture width */
  s32 width = (fontHeader->cell_width * size * vmode->fbWidth) / (fontHeader->cell_height * vmode->viWidth);

  /* adjust texture height */
  size = (size * vmode->efbHeight) / 480;

  /* GX rendering */
  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  GX_Position2s16(xpos, ypos - size);
  GX_Color4u8(color.r, color.g, color.b, 0xff);
  GX_TexCoord2f32(0.0, 0.0);
  GX_Position2s16(xpos + width, ypos - size);
  GX_Color4u8(color.r, color.g, color.b, 0xff);
  GX_TexCoord2f32(1.0, 0.0);
  GX_Position2s16(xpos + width, ypos);
  GX_Color4u8(color.r, color.g, color.b, 0xff);
  GX_TexCoord2f32(1.0, 1.0);
  GX_Position2s16(xpos, ypos);
  GX_Color4u8(color.r, color.g, color.b, 0xff);
  GX_TexCoord2f32(0.0, 1.0);
  GX_End();
  GX_DrawDone();
}

/****************************************************************************
 *  IPL font support
 *
 ****************************************************************************/
extern void __SYS_ReadROM(void *buf,u32 len,u32 offset);

/* Decode one UTF-8 codepoint; malformed sequences fall back to the first byte. */
static unsigned int FONT_NextCodepoint(const unsigned char **s)
{
  const unsigned char *p = *s;
  unsigned int cp;
  if (p[0] < 0x80) { *s = p + 1; return p[0]; }
  if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
  { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); *s = p + 2; return cp; }
  if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
  { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); *s = p + 3; return cp; }
  if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
  { cp = ((p[0] & 7) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); *s = p + 4; return cp; }
  *s = p + 1;
  return p[0];
}

static int FONT_CodepointWidth(unsigned int cp, int size)
{
  if (cp >= 0xAC00 && cp <= 0xD7A3) return size;
  if (cp < 256) return (font_size[cp] * size * vmode->fbWidth) / (fontHeader->cell_height * vmode->viWidth);
  return (font_size[fontHeader->inval_char] * size * vmode->fbWidth) / (fontHeader->cell_height * vmode->viWidth);
}

static void FONT_DrawHangul(unsigned int cp, int xpos, int ypos, int size, GXColor color)
{
  unsigned int idx = cp - 0xAC00;
  const unsigned char *glyph = gx_hangul_16x16[idx];
  int x, y;
  /* Convert 1bpp rows into GX tiled I4 texture layout (16x16 = 128 bytes). */
  memset(hangulTexture, 0, 128);
  for (y = 0; y < 16; ++y)
  for (x = 0; x < 16; ++x)
  {
    unsigned char bit = (glyph[y * 2 + (x >> 3)] >> (7 - (x & 7))) & 1;
    unsigned int off = ((y >> 3) * 64) + ((x >> 3) * 32) + ((x & 7) >> 1) + ((y & 7) << 2);
    if (bit) hangulTexture[off] |= ((x & 1) ? 0x0F : 0xF0);
  }
  DCStoreRange(hangulTexture, 128);
  GX_InvalidateTexAll();
  GX_LoadTexObj(&hangulTexObj, GX_TEXMAP0);
  size = (size * vmode->efbHeight) / 480;
  GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
  GX_Position2s16(xpos, ypos - size); GX_Color4u8(color.r,color.g,color.b,0xff); GX_TexCoord2f32(0,0);
  GX_Position2s16(xpos + size, ypos - size); GX_Color4u8(color.r,color.g,color.b,0xff); GX_TexCoord2f32(1,0);
  GX_Position2s16(xpos + size, ypos); GX_Color4u8(color.r,color.g,color.b,0xff); GX_TexCoord2f32(1,1);
  GX_Position2s16(xpos, ypos); GX_Color4u8(color.r,color.g,color.b,0xff); GX_TexCoord2f32(0,1);
  GX_End();
  GX_DrawDone();
}

static void FONT_DrawCodepoint(unsigned int cp, int x, int y, int size, GXColor color)
{
  if (cp >= 0xAC00 && cp <= 0xD7A3)
    FONT_DrawHangul(cp, x, y, size, color);
  else
  {
    GX_LoadTexObj(&fontTexObj, GX_TEXMAP0);
    DrawChar((unsigned char)(cp < 256 ? cp : fontHeader->inval_char), x, y, size, color);
  }
}

int FONT_Init(void)
{
#ifndef HW_RVL
  /* --- Game Cube --- disable Qoob before accessing IPL */
  ipl_set_config(6);
#endif

  /* read IPL font (ASCII) from Mask ROM */
  ipl_fontarea = memalign(32,131360);
  if (!ipl_fontarea)
    return 0;
  memset(ipl_fontarea,0,131360);
  __SYS_ReadROM(ipl_fontarea+119072,12288,0x1FCF00);

  /* YAY0 decompression */
  decode_szp(ipl_fontarea+119072,ipl_fontarea);

	/* retrieve IPL font data */
  fontHeader = (sys_fontheader*)ipl_fontarea;
  fontImage = (u8*)((((u32)ipl_fontarea+fontHeader->sheet_image)+31)&~31);
  
  /* expand to I4 format */
  expand_font((u8*)ipl_fontarea+fontHeader->sheet_image,fontImage);

  /* character width table */
  int i,c;
  for (i=0; i<256; ++i)
  {
    if ((i < fontHeader->first_char) || (i > fontHeader->last_char))
      c = fontHeader->inval_char;
    else
      c = i - fontHeader->first_char;

    font_size[i] = ((u8*)fontHeader)[fontHeader->width_table + c];
  }

  /* initialize texture data */
  fontTexture = memalign(32, fontHeader->cell_width * fontHeader->cell_height / 2);
  if (!fontTexture)
  {
    free(ipl_fontarea);
    return 0;
  }

  /* initialize texture object */
  GX_InitTexObj(&fontTexObj, fontTexture, fontHeader->cell_width, fontHeader->cell_height, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);

  /* Dedicated 16x16 texture for UTF-8 Hangul syllables. */
  hangulTexture = memalign(32, 128);
  if (!hangulTexture)
  {
    free(fontTexture);
    free(ipl_fontarea);
    fontTexture = NULL;
    ipl_fontarea = NULL;
    fontHeader = NULL;
    return 0;
  }
  memset(hangulTexture, 0, 128);
  GX_InitTexObj(&hangulTexObj, hangulTexture, 16, 16, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);

  return 1;
}

void FONT_Shutdown(void)
{
  if (fontHeader)
    free(ipl_fontarea);
  if (fontTexture)
    free(fontTexture);
  if (hangulTexture)
    free(hangulTexture);
}

int FONT_write(char *string, int size, int x, int y, int max_width, GXColor color)
{
  int w;
  const unsigned char *p = (const unsigned char *)string;
  x -= (vmode->fbWidth / 2);
  y -= (vmode->efbHeight / 2);
  int ox = x;
  GX_LoadTexObj(&fontTexObj, GX_TEXMAP0);
  while (*p)
  {
    unsigned int cp = FONT_NextCodepoint(&p);
    if (cp == '\n') { x = ox; y += size; continue; }
    w = FONT_CodepointWidth(cp, size);
    if ((x + w) > (ox + max_width)) return (int)strlen((const char *)p);
    FONT_DrawCodepoint(cp, x, y, size, color);
    x += w;
  }
  return 0;
}

int FONT_writeCenter(char *string, int size, int x1, int x2, int y, GXColor color)
{
  const unsigned char *p;
  int w, total = 0, x;
  p = (const unsigned char *)string;
  while (*p && *p != '\n') total += FONT_CodepointWidth(FONT_NextCodepoint(&p), size);
  if ((x1 + total) > x2) total = x2 - x1;
  x = x1 + (x2 - x1 - total - vmode->fbWidth) / 2;
  x2 -= (vmode->fbWidth / 2);
  y -= (vmode->efbHeight / 2);
  p = (const unsigned char *)string;
  GX_LoadTexObj(&fontTexObj, GX_TEXMAP0);
  while (*p && *p != '\n')
  {
    unsigned int cp = FONT_NextCodepoint(&p);
    w = FONT_CodepointWidth(cp, size);
    if ((x + w) > x2) return (int)strlen((const char *)p);
    FONT_DrawCodepoint(cp, x, y, size, color);
    x += w;
  }
  if (*p == '\n') return FONT_writeCenter((char *)(p + 1), size, x1, x2 + (vmode->fbWidth / 2), y + size + (vmode->efbHeight / 2), color);
  return 0;
}

int FONT_alignRight(char *string, int size, int x, int y, GXColor color)
{
  const unsigned char *p;
  int total = 0, w, ox;
  p = (const unsigned char *)string;
  while (*p && *p != '\n') total += FONT_CodepointWidth(FONT_NextCodepoint(&p), size);
  x -= (vmode->fbWidth / 2);
  y -= (vmode->efbHeight / 2);
  ox = x;
  x -= total;
  p = (const unsigned char *)string;
  GX_LoadTexObj(&fontTexObj, GX_TEXMAP0);
  while (*p && *p != '\n')
  {
    unsigned int cp = FONT_NextCodepoint(&p);
    w = FONT_CodepointWidth(cp, size);
    if ((x + w) > ox) return (int)strlen((const char *)p);
    FONT_DrawCodepoint(cp, x, y, size, color);
    x += w;
  }
  if (*p == '\n') return FONT_alignRight((char *)(p + 1), size, ox + (vmode->fbWidth / 2), y + size + (vmode->efbHeight / 2), color);
  return 0;
}
