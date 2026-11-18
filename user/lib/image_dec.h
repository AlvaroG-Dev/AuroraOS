// user/lib/image_dec.h — decodificadores de imagen para las apps nativas.
//
//   BMP : 1/4/8 bits con paleta, 16 (5-5-5 / bitfields), 24 y 32 bits, sin
//         comprimir o BI_BITFIELDS; de abajo arriba o de arriba abajo.
//   PPM : P5 (grises) y P6 (color), 8 o 16 bits; P2/P3 en texto.
//   PNG : todos los tipos de color, 1-16 bits, paleta con tRNS, Adam7.
//   JPEG: baseline (Huffman, 8 bits), cualquier submuestreo 1x1..2x2, grises
//         o YCbCr; reinicios (DRI). Los progresivos devuelven error.
//
// Solo cabecera: img_decode() devuelve un búfer malloc() de ancho*alto
// píxeles 0xAARRGGBB (o NULL con un mensaje en img_error). Pensado para
// ficheros de usuario: comprueba límites y no confía en las cabeceras.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define IMG_MAX_PIXELS (64u * 1024u * 1024u) // 64 Mpx

static const char *img_error __attribute__((unused)) = "";

static inline uint32_t img_rd16le(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t img_rd32le(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint32_t img_rd32be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
static inline uint32_t *img_alloc(uint32_t w, uint32_t h) {
  if (!w || !h || (uint64_t)w * h > IMG_MAX_PIXELS) {
    img_error = "imagen demasiado grande o vacía";
    return NULL;
  }
  uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4);
  if (!px)
    img_error = "sin memoria";
  return px;
}

// ===================================================================== BMP
static inline int img_mask_shift(uint32_t m, int *bits) {
  int s = 0, b = 0;
  if (!m) {
    *bits = 0;
    return 0;
  }
  while (!(m & 1)) {
    m >>= 1;
    s++;
  }
  while (m & 1) {
    m >>= 1;
    b++;
  }
  *bits = b;
  return s;
}
static inline uint32_t img_mask_get(uint32_t v, uint32_t m, int s, int b) {
  if (!m)
    return 255;
  uint32_t x = (v & m) >> s;
  if (b >= 8)
    return x >> (b - 8);
  uint32_t max = (1u << b) - 1;
  return max ? x * 255 / max : 0;
}

static inline uint32_t *img_decode_bmp(const uint8_t *d, size_t n, int *ow, int *oh) {
  if (n < 26 || d[0] != 'B' || d[1] != 'M') {
    img_error = "BMP no válido";
    return NULL;
  }
  uint32_t off = img_rd32le(d + 10), hs = img_rd32le(d + 14);
  int32_t w, h;
  int bpp, comp = 0;
  uint32_t ncol = 0;
  if (hs == 12) {
    w = (int16_t)img_rd16le(d + 18);
    h = (int16_t)img_rd16le(d + 20);
    bpp = img_rd16le(d + 24);
  } else if (hs >= 40 && n >= 54) {
    w = (int32_t)img_rd32le(d + 18);
    h = (int32_t)img_rd32le(d + 22);
    bpp = img_rd16le(d + 28);
    comp = (int)img_rd32le(d + 30);
    ncol = img_rd32le(d + 46);
  } else {
    img_error = "cabecera BMP no soportada";
    return NULL;
  }
  int topdown = h < 0;
  if (h < 0)
    h = -h;
  if (w <= 0 || h <= 0 || (comp != 0 && comp != 3 && comp != 6)) {
    img_error = "BMP comprimido (RLE) no soportado";
    return NULL;
  }
  uint32_t rm = 0, gm = 0, bm = 0, am = 0;
  if (comp == 3 || comp == 6) {
    if (hs >= 52 && n >= 14 + 52) {
      rm = img_rd32le(d + 54);
      gm = img_rd32le(d + 58);
      bm = img_rd32le(d + 62);
      if (hs >= 56 && n >= 14 + 56)
        am = img_rd32le(d + 66);
    } else if (n >= 14 + hs + 12) {
      rm = img_rd32le(d + 14 + hs);
      gm = img_rd32le(d + 18 + hs);
      bm = img_rd32le(d + 22 + hs);
    }
  } else if (bpp == 16) {
    rm = 0x7C00;
    gm = 0x03E0;
    bm = 0x001F;
  } else if (bpp == 32) {
    rm = 0x00FF0000;
    gm = 0x0000FF00;
    bm = 0x000000FF;
    am = hs >= 56 ? img_rd32le(d + 66) : 0xFF000000u;
  }
  uint32_t pal[256];
  memset(pal, 0, sizeof(pal));
  if (bpp <= 8) {
    uint32_t pe = hs == 12 ? 3 : 4, maxc = 1u << bpp;
    if (!ncol || ncol > maxc)
      ncol = maxc;
    const uint8_t *p = d + 14 + hs;
    for (uint32_t i = 0; i < ncol && p + pe <= d + n; i++, p += pe)
      pal[i] = 0xFF000000u | (p[2] << 16) | (p[1] << 8) | p[0];
  } else if (bpp != 16 && bpp != 24 && bpp != 32) {
    img_error = "profundidad BMP no soportada";
    return NULL;
  }
  size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
  if (off > n || stride * h > n - off) {
    img_error = "BMP truncado";
    return NULL;
  }
  uint32_t *px = img_alloc(w, h);
  if (!px)
    return NULL;
  int rs, rb, gs, gb, bs, bb, as, ab;
  rs = img_mask_shift(rm, &rb);
  gs = img_mask_shift(gm, &gb);
  bs = img_mask_shift(bm, &bb);
  as = img_mask_shift(am, &ab);
  int any_alpha = 0;
  for (int y = 0; y < h; y++) {
    const uint8_t *row = d + off + stride * (size_t)(topdown ? y : h - 1 - y);
    uint32_t *o = px + (size_t)y * w;
    for (int x = 0; x < w; x++) {
      uint32_t c;
      if (bpp <= 8) {
        int bit = x * bpp;
        int idx = (row[bit >> 3] >> (8 - bpp - (bit & 7))) & ((1 << bpp) - 1);
        c = pal[idx];
      } else if (bpp == 24) {
        const uint8_t *p = row + x * 3;
        c = 0xFF000000u | (p[2] << 16) | (p[1] << 8) | p[0];
      } else {
        uint32_t v = bpp == 16 ? img_rd16le(row + x * 2) : img_rd32le(row + x * 4);
        uint32_t a = am ? img_mask_get(v, am, as, ab) : 255;
        any_alpha |= a != 0;
        c = (a << 24) | (img_mask_get(v, rm, rs, rb) << 16) |
            (img_mask_get(v, gm, gs, gb) << 8) | img_mask_get(v, bm, bs, bb);
      }
      o[x] = c;
    }
  }
  // BMP de 32 bits con alfa todo a 0 (muy habitual): es opaco.
  if (bpp == 32 && am && !any_alpha)
    for (size_t i = 0; i < (size_t)w * h; i++)
      px[i] |= 0xFF000000u;
  *ow = w;
  *oh = h;
  return px;
}

// ===================================================================== PPM
static inline int img_pnm_int(const uint8_t *d, size_t n, size_t *p) {
  for (;;) {
    while (*p < n && (d[*p] == ' ' || d[*p] == '\t' || d[*p] == '\r' || d[*p] == '\n'))
      (*p)++;
    if (*p < n && d[*p] == '#') {
      while (*p < n && d[*p] != '\n')
        (*p)++;
      continue;
    }
    break;
  }
  if (*p >= n || d[*p] < '0' || d[*p] > '9')
    return -1;
  long v = 0;
  while (*p < n && d[*p] >= '0' && d[*p] <= '9') {
    v = v * 10 + (d[*p] - '0');
    if (v > 1 << 24)
      return -1;
    (*p)++;
  }
  return (int)v;
}

static inline uint32_t *img_decode_pnm(const uint8_t *d, size_t n, int *ow, int *oh) {
  if (n < 3 || d[0] != 'P' || (d[1] != '2' && d[1] != '3' && d[1] != '5' && d[1] != '6')) {
    img_error = "PNM no soportado";
    return NULL;
  }
  int color = d[1] == '3' || d[1] == '6', ascii = d[1] == '2' || d[1] == '3';
  size_t p = 2;
  int w = img_pnm_int(d, n, &p), h = img_pnm_int(d, n, &p), mx = img_pnm_int(d, n, &p);
  if (w <= 0 || h <= 0 || mx <= 0 || mx > 65535) {
    img_error = "cabecera PNM no válida";
    return NULL;
  }
  p++; // un blanco tras maxval
  int ch = color ? 3 : 1, bps = mx > 255 ? 2 : 1;
  if (!ascii && (p > n || (size_t)w * h * ch * bps > n - p)) {
    img_error = "PNM truncado";
    return NULL;
  }
  uint32_t *px = img_alloc(w, h);
  if (!px)
    return NULL;
  for (size_t i = 0; i < (size_t)w * h; i++) {
    int v[3];
    for (int c = 0; c < ch; c++) {
      int s;
      if (ascii) {
        s = img_pnm_int(d, n, &p);
        if (s < 0)
          s = 0;
      } else if (bps == 2) {
        s = (d[p] << 8) | d[p + 1];
        p += 2;
      } else {
        s = d[p++];
      }
      v[c] = s * 255 / mx;
      if (v[c] > 255)
        v[c] = 255;
    }
    if (!color)
      v[1] = v[2] = v[0];
    px[i] = 0xFF000000u | (v[0] << 16) | (v[1] << 8) | v[2];
  }
  *ow = w;
  *oh = h;
  return px;
}

// ================================================================= inflate
typedef struct {
  const uint8_t *src;
  size_t n, pos;
  uint32_t bitbuf;
  int bitcnt;
  uint8_t *out;
  size_t outn, outcap;
  int err;
} img_infl_t;

typedef struct {
  uint16_t count[16];
  uint16_t sym[288];
} img_huff_t;

static inline int img_bits(img_infl_t *s, int need) {
  while (s->bitcnt < need) {
    if (s->pos >= s->n) {
      s->err = 1;
      return 0;
    }
    s->bitbuf |= (uint32_t)s->src[s->pos++] << s->bitcnt;
    s->bitcnt += 8;
  }
  int v = (int)(s->bitbuf & ((1u << need) - 1));
  s->bitbuf >>= need;
  s->bitcnt -= need;
  return v;
}

static inline int img_huff_build(img_huff_t *h, const uint8_t *len, int n) {
  uint16_t offs[16];
  memset(h->count, 0, sizeof(h->count));
  for (int i = 0; i < n; i++)
    h->count[len[i]]++;
  h->count[0] = 0;
  int left = 1;
  for (int i = 1; i < 16; i++) {
    left <<= 1;
    left -= h->count[i];
    if (left < 0)
      return -1;
  }
  offs[1] = 0;
  for (int i = 1; i < 15; i++)
    offs[i + 1] = offs[i] + h->count[i];
  for (int i = 0; i < n; i++)
    if (len[i])
      h->sym[offs[len[i]]++] = (uint16_t)i;
  return 0;
}

static inline int img_huff_dec(img_infl_t *s, const img_huff_t *h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; len++) {
    code |= img_bits(s, 1);
    if (s->err)
      return -1;
    int count = h->count[len];
    if (code - count < first)
      return h->sym[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  s->err = 1;
  return -1;
}

static inline int img_out_byte(img_infl_t *s, uint8_t b) {
  if (s->outn >= s->outcap) {
    s->err = 1;
    return -1;
  }
  s->out[s->outn++] = b;
  return 0;
}

static inline int img_inflate_block(img_infl_t *s, const img_huff_t *lit, const img_huff_t *dist) {
  static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                     31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                     193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                     4097, 6145, 8193, 12289, 16385, 24577};
  static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  for (;;) {
    int sym = img_huff_dec(s, lit);
    if (sym < 0 || s->err)
      return -1;
    if (sym < 256) {
      if (img_out_byte(s, (uint8_t)sym))
        return -1;
    } else if (sym == 256) {
      return 0;
    } else {
      sym -= 257;
      if (sym >= 29)
        return -1;
      int len = lbase[sym] + img_bits(s, lext[sym]);
      int ds = img_huff_dec(s, dist);
      if (ds < 0 || ds >= 30)
        return -1;
      size_t dd = dbase[ds] + (size_t)img_bits(s, dext[ds]);
      if (s->err || dd > s->outn)
        return -1;
      while (len--)
        if (img_out_byte(s, s->out[s->outn - dd]))
          return -1;
    }
  }
}

// Descomprime un flujo zlib en out (outcap bytes). Devuelve los bytes escritos o -1.
static inline long img_zlib_inflate(const uint8_t *src, size_t n, uint8_t *out, size_t outcap) {
  if (n < 2 || (src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31 || (src[1] & 0x20))
    return -1;
  img_infl_t s;
  memset(&s, 0, sizeof(s));
  s.src = src + 2;
  s.n = n - 2;
  s.out = out;
  s.outcap = outcap;
  static img_huff_t lit, dist;
  int last;
  do {
    last = img_bits(&s, 1);
    int type = img_bits(&s, 2);
    if (s.err)
      return -1;
    if (type == 0) {
      s.bitbuf = 0;
      s.bitcnt = 0;
      if (s.pos + 4 > s.n)
        return -1;
      unsigned len = s.src[s.pos] | (s.src[s.pos + 1] << 8);
      s.pos += 4;
      if (s.pos + len > s.n || s.outn + len > s.outcap)
        return -1;
      memcpy(s.out + s.outn, s.src + s.pos, len);
      s.outn += len;
      s.pos += len;
    } else if (type == 1) {
      uint8_t l[288];
      int i = 0;
      for (; i < 144; i++) l[i] = 8;
      for (; i < 256; i++) l[i] = 9;
      for (; i < 280; i++) l[i] = 7;
      for (; i < 288; i++) l[i] = 8;
      img_huff_build(&lit, l, 288);
      for (i = 0; i < 30; i++) l[i] = 5;
      img_huff_build(&dist, l, 30);
      if (img_inflate_block(&s, &lit, &dist))
        return -1;
    } else if (type == 2) {
      static const uint8_t ord[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                      11, 4, 12, 3, 13, 2, 14, 1, 15};
      int nlen = img_bits(&s, 5) + 257, ndist = img_bits(&s, 5) + 1, ncode = img_bits(&s, 4) + 4;
      if (s.err || nlen > 286 || ndist > 30)
        return -1;
      uint8_t l[320];
      memset(l, 0, sizeof(l));
      for (int i = 0; i < ncode; i++)
        l[ord[i]] = (uint8_t)img_bits(&s, 3);
      img_huff_t cl;
      if (img_huff_build(&cl, l, 19))
        return -1;
      int i = 0;
      while (i < nlen + ndist) {
        int sym = img_huff_dec(&s, &cl);
        if (sym < 0)
          return -1;
        if (sym < 16) {
          l[i++] = (uint8_t)sym;
        } else {
          int rep, val = 0;
          if (sym == 16) {
            if (!i)
              return -1;
            val = l[i - 1];
            rep = 3 + img_bits(&s, 2);
          } else if (sym == 17) {
            rep = 3 + img_bits(&s, 3);
          } else {
            rep = 11 + img_bits(&s, 7);
          }
          if (i + rep > nlen + ndist)
            return -1;
          while (rep--)
            l[i++] = (uint8_t)val;
        }
      }
      if (s.err || img_huff_build(&lit, l, nlen))
        return -1;
      if (img_huff_build(&dist, l + nlen, ndist) && ndist > 1)
        return -1;
      if (img_inflate_block(&s, &lit, &dist))
        return -1;
    } else {
      return -1;
    }
  } while (!last);
  return (long)s.outn;
}

// ===================================================================== PNG
static inline int img_paeth(int a, int b, int c) {
  int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// Quita los filtros de una imagen (o pasada Adam7) de w x h con bpp bits/píxel.
static inline int img_png_unfilter(uint8_t *raw, size_t rawn, uint8_t *dst, int w, int h,
                                   int bitpp) {
  size_t rb = ((size_t)w * bitpp + 7) / 8;
  int bpp = bitpp >= 8 ? bitpp / 8 : 1;
  if (rawn < (rb + 1) * h)
    return -1;
  for (int y = 0; y < h; y++) {
    uint8_t f = raw[y * (rb + 1)];
    uint8_t *in = raw + y * (rb + 1) + 1;
    uint8_t *cur = dst + y * rb, *prev = y ? dst + (y - 1) * rb : NULL;
    for (size_t i = 0; i < rb; i++) {
      int a = i >= (size_t)bpp ? cur[i - bpp] : 0;
      int b = prev ? prev[i] : 0;
      int c = prev && i >= (size_t)bpp ? prev[i - bpp] : 0;
      int v = in[i];
      switch (f) {
      case 0: break;
      case 1: v += a; break;
      case 2: v += b; break;
      case 3: v += (a + b) >> 1; break;
      case 4: v += img_paeth(a, b, c); break;
      default: return -1;
      }
      cur[i] = (uint8_t)v;
    }
  }
  return 0;
}

static inline uint32_t img_png_pixel(const uint8_t *row, int x, int ct, int depth,
                                     const uint32_t *pal, int trns_on, const uint16_t *trns) {
  int ch = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : 4;
  uint32_t s[4];
  if (depth < 8) {
    int bit = x * depth;
    uint32_t v = (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
    if (ct == 3)
      return pal[v];
    if (trns_on && v == trns[0])
      return 0;
    v = v * 255 / ((1u << depth) - 1);
    return 0xFF000000u | (v << 16) | (v << 8) | v;
  }
  for (int c = 0; c < ch; c++)
    s[c] = depth == 16 ? (uint32_t)((row[(x * ch + c) * 2] << 8) | row[(x * ch + c) * 2 + 1])
                       : row[x * ch + c];
  if (ct == 3)
    return pal[s[0] & 0xFF];
  int sh = depth == 16 ? 8 : 0;
  switch (ct) {
  case 0: {
    if (trns_on && s[0] == trns[0])
      return 0;
    uint32_t g = s[0] >> sh;
    return 0xFF000000u | (g << 16) | (g << 8) | g;
  }
  case 2:
    if (trns_on && s[0] == trns[0] && s[1] == trns[1] && s[2] == trns[2])
      return 0;
    return 0xFF000000u | ((s[0] >> sh) << 16) | ((s[1] >> sh) << 8) | (s[2] >> sh);
  case 4: {
    uint32_t g = s[0] >> sh;
    return ((s[1] >> sh) << 24) | (g << 16) | (g << 8) | g;
  }
  default:
    return ((s[3] >> sh) << 24) | ((s[0] >> sh) << 16) | ((s[1] >> sh) << 8) | (s[2] >> sh);
  }
}

static inline uint32_t *img_decode_png(const uint8_t *d, size_t n, int *ow, int *oh) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  if (n < 33 || memcmp(d, sig, 8)) {
    img_error = "PNG no válido";
    return NULL;
  }
  uint32_t w = 0, h = 0;
  int depth = 0, ct = 0, il = 0, trns_on = 0;
  uint32_t pal[256];
  uint16_t trns[3] = {0, 0, 0};
  for (int i = 0; i < 256; i++)
    pal[i] = 0xFF000000u;
  uint8_t *idat = NULL;
  size_t idn = 0, idcap = 0;
  size_t p = 8;
  while (p + 12 <= n) {
    uint32_t len = img_rd32be(d + p);
    const uint8_t *type = d + p + 4, *cd = d + p + 8;
    if (len > n - p - 12)
      break;
    if (!memcmp(type, "IHDR", 4) && len >= 13) {
      w = img_rd32be(cd);
      h = img_rd32be(cd + 4);
      depth = cd[8];
      ct = cd[9];
      il = cd[12];
    } else if (!memcmp(type, "PLTE", 4)) {
      for (uint32_t i = 0; i < len / 3 && i < 256; i++)
        pal[i] = 0xFF000000u | (cd[i * 3] << 16) | (cd[i * 3 + 1] << 8) | cd[i * 3 + 2];
    } else if (!memcmp(type, "tRNS", 4)) {
      if (ct == 3) {
        for (uint32_t i = 0; i < len && i < 256; i++)
          pal[i] = (pal[i] & 0xFFFFFF) | ((uint32_t)cd[i] << 24);
      } else if (ct == 0 && len >= 2) {
        trns[0] = (uint16_t)((cd[0] << 8) | cd[1]);
        trns_on = 1;
      } else if (ct == 2 && len >= 6) {
        for (int c = 0; c < 3; c++)
          trns[c] = (uint16_t)((cd[c * 2] << 8) | cd[c * 2 + 1]);
        trns_on = 1;
      }
    } else if (!memcmp(type, "IDAT", 4)) {
      if (idn + len > idcap) {
        size_t nc = (idn + len) * 2 + 4096;
        uint8_t *ni = (uint8_t *)realloc(idat, nc);
        if (!ni) {
          free(idat);
          img_error = "sin memoria";
          return NULL;
        }
        idat = ni;
        idcap = nc;
      }
      memcpy(idat + idn, cd, len);
      idn += len;
    } else if (!memcmp(type, "IEND", 4)) {
      break;
    }
    p += 12 + len;
  }
  int ch = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
  int okdepth = depth == 8 || depth == 16 || ((ct == 0 || ct == 3) && (depth == 1 || depth == 2 || depth == 4));
  if (!w || !h || !ch || !okdepth || (ct == 3 && depth == 16) || !idat) {
    free(idat);
    img_error = "PNG no soportado";
    return NULL;
  }
  if ((uint64_t)w * h > IMG_MAX_PIXELS) {
    free(idat);
    img_error = "imagen demasiado grande";
    return NULL;
  }
  int bitpp = ch * depth;
  // Tamaño descomprimido: suma de las pasadas (o la imagen entera) + filtros.
  static const int ax0[7] = {0, 4, 0, 2, 0, 1, 0}, ay0[7] = {0, 0, 4, 0, 2, 0, 1};
  static const int adx[7] = {8, 8, 4, 4, 2, 2, 1}, ady[7] = {8, 8, 8, 4, 4, 2, 2};
  size_t rawcap = 0;
  int npass = il ? 7 : 1;
  for (int k = 0; k < npass; k++) {
    uint32_t pw = il ? (w - ax0[k] + adx[k] - 1) / adx[k] : w;
    uint32_t ph = il ? (h - ay0[k] + ady[k] - 1) / ady[k] : h;
    if (w <= (uint32_t)ax0[k] || h <= (uint32_t)ay0[k])
      pw = ph = 0;
    if (pw && ph)
      rawcap += ((size_t)pw * bitpp + 7) / 8 * ph + ph;
  }
  uint8_t *raw = (uint8_t *)malloc(rawcap + 1);
  uint32_t *px = raw ? img_alloc(w, h) : NULL;
  uint8_t *unf = px ? (uint8_t *)malloc((((size_t)w * bitpp + 7) / 8) * h + 1) : NULL;
  long got = unf ? img_zlib_inflate(idat, idn, raw, rawcap) : -1;
  free(idat);
  if (got < 0) {
    if (unf)
      img_error = "PNG dañado (zlib)";
    free(raw);
    free(px);
    free(unf);
    return NULL;
  }
  size_t rp = 0;
  for (int k = 0; k < npass; k++) {
    uint32_t pw = il ? (w - ax0[k] + adx[k] - 1) / adx[k] : w;
    uint32_t ph = il ? (h - ay0[k] + ady[k] - 1) / ady[k] : h;
    if (w <= (uint32_t)ax0[k] || h <= (uint32_t)ay0[k])
      pw = ph = 0;
    if (!pw || !ph)
      continue;
    size_t rb = ((size_t)pw * bitpp + 7) / 8;
    if (img_png_unfilter(raw + rp, (size_t)got - (rp < (size_t)got ? rp : (size_t)got), unf,
                         (int)pw, (int)ph, bitpp)) {
      free(raw);
      free(px);
      free(unf);
      img_error = "PNG dañado (filtro)";
      return NULL;
    }
    rp += (rb + 1) * ph;
    for (uint32_t y = 0; y < ph; y++)
      for (uint32_t x = 0; x < pw; x++) {
        uint32_t X = il ? ax0[k] + x * adx[k] : x, Y = il ? ay0[k] + y * ady[k] : y;
        px[(size_t)Y * w + X] = img_png_pixel(unf + y * rb, (int)x, ct, depth, pal, trns_on, trns);
      }
  }
  free(raw);
  free(unf);
  *ow = (int)w;
  *oh = (int)h;
  return px;
}

// ==================================================================== JPEG
typedef struct {
  uint8_t bits[17];
  uint8_t val[256];
  uint16_t mincode[17];
  int32_t maxcode[18];
  int valptr[17];
} img_jhuff_t;

typedef struct {
  const uint8_t *d;
  size_t n, p;
  uint32_t buf;
  int cnt;
  int marker; // marcador encontrado dentro de los datos
} img_jbits_t;

static inline void img_jhuff_build(img_jhuff_t *h) {
  int code = 0, k = 0;
  for (int l = 1; l <= 16; l++) {
    h->valptr[l] = k;
    h->mincode[l] = (uint16_t)code;
    code += h->bits[l];
    k += h->bits[l];
    h->maxcode[l] = h->bits[l] ? code - 1 : -1;
    code <<= 1;
  }
  h->maxcode[17] = 0x7FFFFFFF;
}

static inline int img_jbit(img_jbits_t *b) {
  if (b->cnt == 0) {
    uint8_t c = 0;
    if (!b->marker && b->p < b->n) {
      c = b->d[b->p++];
      if (c == 0xFF) {
        uint8_t c2 = b->p < b->n ? b->d[b->p] : 0;
        if (c2 == 0)
          b->p++;
        else {
          b->marker = c2;
          c = 0;
        }
      }
    }
    b->buf = c;
    b->cnt = 8;
  }
  b->cnt--;
  return (b->buf >> b->cnt) & 1;
}

static inline int img_jbits_get(img_jbits_t *b, int n) {
  int v = 0;
  while (n--)
    v = (v << 1) | img_jbit(b);
  return v;
}

static inline int img_jdecode(img_jbits_t *b, const img_jhuff_t *h) {
  int code = img_jbit(b);
  for (int l = 1; l <= 16; l++) {
    if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l])
      return h->val[(h->valptr[l] + code - h->mincode[l]) & 0xFF];
    code = (code << 1) | img_jbit(b);
  }
  return -1;
}

static inline int img_jextend(int v, int t) { return t && v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

// IDCT 8x8 separable (flotante; suficiente para un visor).
static inline void img_jidct(const int *in, uint8_t *out, int stride) {
  static float c[8][8];
  static int init;
  if (!init) {
    for (int x = 0; x < 8; x++)
      for (int u = 0; u < 8; u++) {
        float cu = u == 0 ? 0.70710678f : 1.0f;
        c[x][u] = cu * (float)cos((2 * x + 1) * u * 3.14159265358979 / 16.0);
      }
    init = 1;
  }
  float tmp[64];
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      float s = 0;
      for (int u = 0; u < 8; u++)
        s += c[x][u] * (float)in[y * 8 + u];
      tmp[y * 8 + x] = s / 2;
    }
  for (int x = 0; x < 8; x++)
    for (int y = 0; y < 8; y++) {
      float s = 0;
      for (int v = 0; v < 8; v++)
        s += c[y][v] * tmp[v * 8 + x];
      int r = (int)(s / 2 + 128.5f);
      out[y * stride + x] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    }
}

static inline uint32_t *img_decode_jpeg(const uint8_t *d, size_t n, int *ow, int *oh) {
  static const uint8_t zz[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) {
    img_error = "JPEG no válido";
    return NULL;
  }
  static uint16_t qt[4][64];
  static img_jhuff_t hdc[4], hac[4];
  int W = 0, H = 0, ncomp = 0, restart = 0;
  struct {
    int id, h, v, tq, td, ta, pred;
    uint8_t *plane;
    int pw, ph;
  } cp[3];
  memset(cp, 0, sizeof(cp));
  size_t p = 2;
  uint32_t *px = NULL;
  while (p + 4 <= n) {
    if (d[p] != 0xFF) {
      p++;
      continue;
    }
    int m = d[p + 1];
    p += 2;
    if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0xFF) {
      if (m == 0xFF)
        p--;
      continue;
    }
    if (m == 0xD9)
      break;
    if (p + 2 > n)
      break;
    size_t len = (d[p] << 8) | d[p + 1];
    if (len < 2 || p + len > n)
      break;
    const uint8_t *s = d + p + 2;
    size_t sl = len - 2;
    if (m == 0xDB) {
      for (size_t i = 0; i < sl;) {
        int pq = s[i] >> 4, tq = s[i] & 3;
        i++;
        for (int k = 0; k < 64 && i < sl; k++) {
          qt[tq][zz[k]] = pq ? (uint16_t)((s[i] << 8) | s[i + 1]) : s[i];
          i += pq ? 2 : 1;
        }
      }
    } else if (m == 0xC4) {
      for (size_t i = 0; i + 17 <= sl;) {
        int tc = s[i] >> 4, th = s[i] & 3;
        img_jhuff_t *h = tc ? &hac[th] : &hdc[th];
        int total = 0;
        h->bits[0] = 0;
        for (int l = 1; l <= 16; l++)
          total += h->bits[l] = s[i + l];
        i += 17;
        if (total > 256 || i + total > sl)
          break;
        memcpy(h->val, s + i, total);
        i += total;
        img_jhuff_build(h);
      }
    } else if (m == 0xDD && sl >= 2) {
      restart = (s[0] << 8) | s[1];
    } else if (m == 0xC0 || m == 0xC1) {
      if (sl < 6)
        break;
      H = (s[1] << 8) | s[2];
      W = (s[3] << 8) | s[4];
      ncomp = s[5];
      if ((ncomp != 1 && ncomp != 3) || sl < 6 + 3 * (size_t)ncomp) {
        img_error = "JPEG con componentes no soportados";
        return NULL;
      }
      for (int c = 0; c < ncomp; c++) {
        cp[c].id = s[6 + c * 3];
        cp[c].h = s[7 + c * 3] >> 4;
        cp[c].v = s[7 + c * 3] & 15;
        cp[c].tq = s[8 + c * 3] & 3;
        if (cp[c].h < 1 || cp[c].h > 2 || cp[c].v < 1 || cp[c].v > 2) {
          img_error = "submuestreo JPEG no soportado";
          return NULL;
        }
      }
    } else if (m == 0xC2 || m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) {
      img_error = "JPEG progresivo o sin pérdida: no soportado";
      return NULL;
    } else if (m == 0xDA) {
      if (!W || !H || sl < 1) {
        img_error = "JPEG sin cabecera de cuadro";
        return NULL;
      }
      int ns = s[0];
      for (int i = 0; i < ns && 1 + i * 2 + 1 < (int)sl; i++)
        for (int c = 0; c < ncomp; c++)
          if (cp[c].id == s[1 + i * 2]) {
            cp[c].td = s[2 + i * 2] >> 4 & 3;
            cp[c].ta = s[2 + i * 2] & 3;
          }
      int hmax = 1, vmax = 1;
      for (int c = 0; c < ncomp; c++) {
        if (cp[c].h > hmax) hmax = cp[c].h;
        if (cp[c].v > vmax) vmax = cp[c].v;
      }
      int mcux = (W + 8 * hmax - 1) / (8 * hmax), mcuy = (H + 8 * vmax - 1) / (8 * vmax);
      if ((uint64_t)W * H > IMG_MAX_PIXELS) {
        img_error = "imagen demasiado grande";
        return NULL;
      }
      for (int c = 0; c < ncomp; c++) {
        cp[c].pw = mcux * cp[c].h * 8;
        cp[c].ph = mcuy * cp[c].v * 8;
        cp[c].plane = (uint8_t *)malloc((size_t)cp[c].pw * cp[c].ph);
        cp[c].pred = 0;
        if (!cp[c].plane) {
          for (int k = 0; k < c; k++)
            free(cp[k].plane);
          img_error = "sin memoria";
          return NULL;
        }
      }
      img_jbits_t b = {d, n, p + len, 0, 0, 0};
      int blk[64];
      int mcus = 0, bad = 0;
      for (int my = 0; my < mcuy && !bad; my++)
        for (int mx = 0; mx < mcux && !bad; mx++) {
          if (restart && mcus && mcus % restart == 0) {
            // Saltar al marcador RSTn y reiniciar predictores.
            b.cnt = 0;
            if (b.marker >= 0xD0 && b.marker <= 0xD7) {
              b.p++;
              b.marker = 0;
            } else {
              while (b.p + 1 < b.n && !(b.d[b.p] == 0xFF && b.d[b.p + 1] >= 0xD0 && b.d[b.p + 1] <= 0xD7))
                b.p++;
              b.p += 2;
              b.marker = 0;
            }
            for (int c = 0; c < ncomp; c++)
              cp[c].pred = 0;
          }
          mcus++;
          for (int c = 0; c < ncomp && !bad; c++)
            for (int by = 0; by < cp[c].v; by++)
              for (int bx = 0; bx < cp[c].h; bx++) {
                memset(blk, 0, sizeof(blk));
                int t = img_jdecode(&b, &hdc[cp[c].td]);
                if (t < 0 || t > 16) {
                  bad = 1;
                  break;
                }
                int diff = t ? img_jextend(img_jbits_get(&b, t), t) : 0;
                cp[c].pred += diff;
                blk[0] = cp[c].pred * qt[cp[c].tq][0];
                for (int k = 1; k < 64;) {
                  int rs = img_jdecode(&b, &hac[cp[c].ta]);
                  if (rs < 0) {
                    bad = 1;
                    break;
                  }
                  int r = rs >> 4, sz = rs & 15;
                  if (!sz) {
                    if (r != 15)
                      break;
                    k += 16;
                    continue;
                  }
                  k += r;
                  if (k > 63) {
                    bad = 1;
                    break;
                  }
                  blk[zz[k]] = img_jextend(img_jbits_get(&b, sz), sz) * qt[cp[c].tq][zz[k]];
                  k++;
                }
                if (bad)
                  break;
                int ox = (mx * cp[c].h + bx) * 8, oy = (my * cp[c].v + by) * 8;
                img_jidct(blk, cp[c].plane + (size_t)oy * cp[c].pw + ox, cp[c].pw);
              }
        }
      px = img_alloc(W, H);
      if (px) {
        for (int y = 0; y < H; y++)
          for (int x = 0; x < W; x++) {
            int v[3];
            for (int c = 0; c < ncomp; c++) {
              int sx = x * cp[c].h / hmax, sy = y * cp[c].v / vmax;
              v[c] = cp[c].plane[(size_t)sy * cp[c].pw + sx];
            }
            int R, G, B;
            if (ncomp == 1) {
              R = G = B = v[0];
            } else {
              int Y = v[0], cb = v[1] - 128, cr = v[2] - 128;
              R = Y + ((91881 * cr) >> 16);
              G = Y - ((22554 * cb + 46802 * cr) >> 16);
              B = Y + ((116130 * cb) >> 16);
              R = R < 0 ? 0 : R > 255 ? 255 : R;
              G = G < 0 ? 0 : G > 255 ? 255 : G;
              B = B < 0 ? 0 : B > 255 ? 255 : B;
            }
            px[(size_t)y * W + x] = 0xFF000000u | (R << 16) | (G << 8) | B;
          }
      }
      for (int c = 0; c < ncomp; c++)
        free(cp[c].plane);
      if (px) {
        *ow = W;
        *oh = H;
      }
      return px;
    }
    p += len;
  }
  img_error = "JPEG sin datos de imagen";
  return NULL;
}

// Detecta el formato por la firma y decodifica.
static inline uint32_t *img_decode(const uint8_t *d, size_t n, int *w, int *h) {
  img_error = "formato desconocido";
  if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
    return img_decode_png(d, n, w, h);
  if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8)
    return img_decode_jpeg(d, n, w, h);
  if (n >= 2 && d[0] == 'B' && d[1] == 'M')
    return img_decode_bmp(d, n, w, h);
  if (n >= 2 && d[0] == 'P' && d[1] >= '2' && d[1] <= '6')
    return img_decode_pnm(d, n, w, h);
  return NULL;
}

// ¿Tiene `name` una extensión de imagen que img_decode() entiende?
static inline int img_is_image_name(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot)
    return 0;
  static const char *ext[] = {"png", "jpg", "jpeg", "jpe", "bmp", "ppm", "pgm", "pnm", "dib"};
  for (unsigned i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
    const char *a = dot + 1, *b = ext[i];
    while (*a && *b && ((*a | 32) == *b)) {
      a++;
      b++;
    }
    if (!*a && !*b)
      return 1;
  }
  return 0;
}
