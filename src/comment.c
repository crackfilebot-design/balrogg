/*  Copyright (C) 2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "comment.h"
#include "rc.h"

#define C_BLOCK 16384
#define C_WINDOW 65536
#define C_HASH 65536
#define C_MATCH 259
#define C_CONTEXTS (C_PAD + 1)

enum { C_VENDOR, C_KEY, C_VALUE, C_TAIL, C_OPAQUE, C_PICTURE, C_ROLES };
#define C_FLAG  (C_ROLES * 16)
#define C_MLEN  (C_FLAG + C_ROLES)
#define C_DIST  (C_MLEN + C_ROLES)
#define C_SIZE  (C_DIST + 2)
#define C_COUNT (C_SIZE + 4)
#define C_SPLIT (C_COUNT + 4)
#define C_FORM  (C_SPLIT + 1)
#define C_PAD   (C_FORM + 1)

/*  Dictionary of common metadata KV pairs.  */
static const char dict[] =
  "TITLE=\0ARTIST=\0ALBUM=\0ALBUMARTIST=\0DATE=\0GENRE=\0TRACKNUMBER=\0"
  "TRACKTOTAL=\0DISCNUMBER=\0DISCTOTAL=\0COMMENT=\0DESCRIPTION=\0"
  "COMPOSER=\0PERFORMER=\0COPYRIGHT=\0LICENSE=\0ENCODER=\0"
  "METADATA_BLOCK_PICTURE=\0REPLAYGAIN_TRACK_GAIN=\0REPLAYGAIN_TRACK_PEAK=\0"
  "REPLAYGAIN_ALBUM_GAIN=\0REPLAYGAIN_ALBUM_PEAK=\0R128_TRACK_GAIN=\0"
  "R128_ALBUM_GAIN=\0MUSICBRAINZ_TRACKID=\0MUSICBRAINZ_ALBUMID=\0"
  "MUSICBRAINZ_ARTISTID=\0MUSICBRAINZ_ALBUMARTISTID=\0"
  "Xiph.Org libVorbis I \0libopus\0Lavf\0Lavc\0https://\0http://\0"
  "image/jpeg\0image/png\0Front cover\0";
static const char b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

typedef struct {
  cmt_io * io;
  u16 p[C_CONTEXTS][256];
  u8 count[C_CONTEXTS][256];
  u8 win[C_WINDOW], buf[C_BLOCK], out[C_BLOCK];
  u32 * hash, pos;
  sz used;
} cmt;

static u32 symbol(cmt * c, int ctx, int bits, u32 v) {
  u32 idx = 1;
  int k;
  for (k = bits - 1; k >= 0; k--) {
    int b = c->io->bit(c->io->coder, c->p[ctx][idx], (int) (v >> k & 1));
    c->p[ctx][idx] = rc_adapt(c->p[ctx][idx], &c->count[ctx][idx], RC_ALIM, b);
    idx = idx * 2 + (u32) b;
  }
  return idx - (1UL << bits);
}

static void emit(cmt * c, const u8 * b, sz n) {
  if (c->io->enc) return;
  while (n) {
    sz take = MIN(n, C_BLOCK - c->used);
    memcpy(c->out + c->used, b, take);  c->used += take;
    b += take;  n -= take;
    if (c->used == C_BLOCK) {
      c->io->write(c->io->output, c->out, c->used);  c->used = 0;
    }
  }
}

static u32 le32(cmt_io * f, sz at) {
  u8 b[4];
  f->read(f->input, at, b, 4);
  return (u32) b[0] | (u32) b[1] << 8 | (u32) b[2] << 16 | (u32) b[3] << 24;
}

/*  Check all lengths before choosing structured coding.  */
static int structured(cmt_io * f, sz n, int opus) {
  u8 magic[8];
  sz at = opus ? 8 : 7;
  u32 len, count, i;
  if (n < at + 8) return 0;
  f->read(f->input, 0, magic, at);
  if (memcmp(magic, opus ? "OpusTags" : "\003vorbis", at)) return 0;
  len = le32(f, at);  at += 4;
  if (len > n - at - 4) return 0;
  at += len;  count = le32(f, at);  at += 4;
  if (count > (n - at) / 4) return 0;
  Fi(count,
    if (n - at < 4) return 0;
    len = le32(f, at);  at += 4;
    if (len > n - at) return 0;
    at += len);
  return 1;
}

static u32 number(cmt * c, int ctx, u32 v) {
  u32 x = 0;
  int i;
  Fi(4, x |= symbol(c, ctx + i, 8, v >> (8 * i) & 255) << (8 * i));
  return x;
}

static u32 field(cmt * c, sz * at, sz n, int ctx) {
  u32 v;
  u8 b[4];
  int i;
  FATAL_UNLESS(n - *at >= 4, "comment: truncated length");
  v = number(c, ctx, c->io->enc ? le32(c->io, *at) : 0);
  Fi(4, b[i] = (u8) (v >> (8 * i)));
  emit(c, b, 4);  *at += 4;
  return v;
}

static u32 hash4(const u8 * b) {
  u32 h = (u32) b[0] * 0x1E35A7BDUL ^ (u32) b[1] * 0x9E3779B1UL;
  h ^= (u32) b[2] << 8 | b[3];
  return (h ^ (h >> 16)) & (C_HASH - 1);
}

/*  Matches may overlap, but never cross a block or reach beyond history.  */
static void text(cmt * c, sz at, sz n, int role) {
  int prev = 0;
  while (n) {
    sz take = MIN(n, C_BLOCK), i = 0;
    if (c->io->enc) c->io->read(c->io->input, at, c->buf, take);
    while (i < take) {
      u32 dist = 0, len = 0, j;
      int match;
      if (c->io->enc && take - i >= 4) {
        u32 p = c->hash[hash4(c->buf + i)];
        if (p && c->pos - (p - 1) <= C_WINDOW) {
          dist = c->pos - (p - 1);
          while (len < C_MATCH && len < take - i) {
            u8 b = len < dist ? c->win[(p - 1 + len) & (C_WINDOW - 1)]
                               : c->buf[i + len - dist];
            if (b != c->buf[i + len]) break;
            len++;
          }
        }
      }
      match = (int) symbol(c, C_FLAG + role, 1, len >= 4);
      if (match) {
        len = 4 + symbol(c, C_MLEN + role, 8, len >= 4 ? len - 4 : 0);
        {
          u32 d = dist ? dist - 1 : 0;
          u32 lo = symbol(c, C_DIST, 8, d & 255);
          u32 hi = symbol(c, C_DIST + 1, 8, d >> 8);
          dist = 1 + (lo | hi << 8);
        }
        FATAL_UNLESS(dist && dist <= MIN(c->pos, C_WINDOW) && len <= take - i,
                     "comment: invalid text match");
      } else {
        c->buf[i] = (u8) symbol(c, role * 16 + (prev >> 4), 8,
                                 c->io->enc ? c->buf[i] : 0);
        len = 1;
      }
      Fj(len,
        if (c->io->enc) {
          if (i + j + 4 <= take) c->hash[hash4(c->buf + i + j)] = c->pos + 1;
        } else if (match) c->buf[i + j] = c->win[(c->pos - dist) & (C_WINDOW - 1)];
        c->win[c->pos++ & (C_WINDOW - 1)] = c->buf[i + j]);
      prev = c->buf[i + len - 1];  i += len;
    }
    emit(c, c->buf, take);  at += take;  n -= take;
  }
}

static int un64(u8 b) {
  if (b >= 'A' && b <= 'Z') return b - 'A';
  if (b >= 'a' && b <= 'z') return b - 'a' + 26;
  if (b >= '0' && b <= '9') return b - '0' + 52;
  if (b == '+') return 62;
  if (b == '/') return 63;
  return -1;
}

static int base64(cmt_io * f, sz at, sz n, u32 * bytes) {
  u8 buf[4096];
  sz left = n, i;
  int pad = 0;
  if (!n || n % 4) return 0;
  while (left) {
    sz take = MIN(left, sizeof buf);
    f->read(f->input, at, buf, take);
    for (i = 0; i < take; i += 4) {
      int a = un64(buf[i]), b = un64(buf[i + 1]);
      int d = un64(buf[i + 2]), e = un64(buf[i + 3]);
      if (a < 0 || b < 0) return 0;
      if (left == take && i + 4 == take && buf[i + 3] == '=') {
        pad = buf[i + 2] == '=' ? 2 : 1;
        if (pad == 2 ? (b & 15) != 0 : d < 0 || (d & 3) != 0) return 0;
      } else if (d < 0 || e < 0) return 0;
    }
    at += take;  left -= take;
  }
  *bytes = (u32) (n / 4 * 3 - (sz) pad);
  return 1;
}

/*  Read a big-endian picture field directly through canonical base64.  */
static u32 pic32(cmt_io * f, sz at, u32 off) {
  u8 b[8], raw[6];
  sz i, take = 8;
  f->read(f->input, at + off / 3 * 4, b, take);
  for (i = 0; i < take; i += 4) {
    int a = un64(b[i]), v = un64(b[i + 1]);
    int d = un64(b[i + 2]), e = un64(b[i + 3]);
    raw[i / 4 * 3] = (u8) (a << 2 | v >> 4);
    raw[i / 4 * 3 + 1] = (u8) (v << 4 | (d < 0 ? 0 : d >> 2));
    raw[i / 4 * 3 + 2] = (u8) ((d < 0 ? 0 : d << 6) | (e < 0 ? 0 : e));
  }
  i = off % 3;
  return (u32) raw[i] << 24 | (u32) raw[i + 1] << 16 |
         (u32) raw[i + 2] << 8 | raw[i + 3];
}

static int picture(cmt_io * f, sz at, sz n, u32 * bytes, u32 * head) {
  u32 v, p;
  if (!base64(f, at, n, bytes) || *bytes < 32) return 0;
  v = pic32(f, at, 4);
  if (v > *bytes - 32) return 0;
  p = 8 + v;  v = pic32(f, at, p);
  if (v > *bytes - p - 24) return 0;
  p += 4 + v + 16;
  v = pic32(f, at, p);
  *head = p + 4;
  return v == *bytes - *head;
}

static int picture_key(const u8 * b, sz n) {
  static const char key[] = "METADATA_BLOCK_PICTURE=";
  sz i;
  if (n != sizeof key - 1) return 0;
  Fi(n,
    int v = b[i];
    if (v >= 'a' && v <= 'z') v -= 'a' - 'A';
    if (v != key[i]) return 0);
  return 1;
}

/*  Picture metadata uses byte contexts; compressed image bytes use uniform
    bits.  Original base64 is restored when decoding.  */
static void image(cmt * c, sz at, sz n, u32 bytes, u32 head) {
  u32 pad = c->io->enc ? (u32) (n / 4 * 3) - bytes : 0;
  u32 i, prev = 0;
  pad = symbol(c, C_PAD, 2, pad);
  FATAL_UNLESS(n && n % 4 == 0 && pad <= 2, "comment: invalid base64 length");
  bytes = (u32) (n / 4 * 3) - pad;
  head = number(c, C_SIZE, head);
  FATAL_UNLESS(head >= 32 && head <= bytes, "comment: invalid picture header");
  for (i = 0; i < bytes; i += 3) {
    u8 b[4] = { 0, 0, 0, 0 }, raw[3] = { 0, 0, 0 };
    u32 j, take = MIN(3, bytes - i);
    if (c->io->enc) {
      int a, v, d, e;
      sz off = (sz) i / 3 * 4;
      if (!(off % C_BLOCK))
        c->io->read(c->io->input, at + off, c->buf, MIN(C_BLOCK, n - off));
      memcpy(b, c->buf + off % C_BLOCK, 4);
      a = un64(b[0]);  v = un64(b[1]);  d = un64(b[2]);  e = un64(b[3]);
      raw[0] = (u8) (a << 2 | v >> 4);
      raw[1] = (u8) (v << 4 | (d < 0 ? 0 : d >> 2));
      raw[2] = (u8) ((d < 0 ? 0 : d << 6) | (e < 0 ? 0 : e));
    }
    Fj(take,
      if (i + j < head) raw[j] = (u8) symbol(c, C_PICTURE * 16 + (int) (prev >> 4), 8, raw[j]);
      else {
        u32 v = 0;
        int k;
        for (k = 7; k >= 0; k--)
          v = v * 2 + (u32) c->io->bit(c->io->coder, RC_PINIT, raw[j] >> k & 1);
        raw[j] = (u8) v;
      }
      prev = raw[j]);
    b[0] = (u8) b64[raw[0] >> 2];
    b[1] = (u8) b64[(raw[0] & 3) << 4 | raw[1] >> 4];
    b[2] = take > 1 ? (u8) b64[(raw[1] & 15) << 2 | raw[2] >> 6] : '=';
    b[3] = take > 2 ? (u8) b64[raw[2] & 63] : '=';
    emit(c, b, 4);
  }
}

void cmt_code(cmt_io * io, sz n, int opus) {
  cmt * c = xcalloc(1, sizeof *c);
  sz at = opus ? 8 : 7, j;
  u32 len, count, i;
  int form;
  FATAL_UNLESS(n <= CMT_MAXLEN, "comment: packet exceeds 120 MiB");
  c->io = io;
  rc_adapt_init();  rc_probs_init(&c->p[0][0], C_CONTEXTS * 256);
  memcpy(c->win, dict, sizeof dict - 1);  c->pos = sizeof dict - 1;
  if (io->enc) {
    c->hash = xcalloc(C_HASH, sizeof *c->hash);
    Fj(sizeof dict - 4, c->hash[hash4(c->win + j)] = (u32) j + 1);
  }
  form = (int) symbol(c, C_FORM, 1, io->enc ? (u32) structured(io, n, opus) : 0);
  if (!form) text(c, 0, n, C_OPAQUE);
  else {
    FATAL_UNLESS(n >= at + 8, "comment: truncated header");
    emit(c, (const u8 *) (opus ? "OpusTags" : "\003vorbis"), at);
    len = field(c, &at, n, C_SIZE);
    FATAL_UNLESS(len <= n - at - 4, "comment: invalid vendor length");
    text(c, at, len, C_VENDOR);  at += len;
    count = field(c, &at, n, C_COUNT);
    FATAL_UNLESS(count <= (n - at) / 4, "comment: invalid field count");
    Fi(count,
      u8 key[255];
      u32 split = 0, bytes = 0, head = 0;
      int pic = 0;
      len = field(c, &at, n, C_SIZE);
      FATAL_UNLESS(len <= n - at, "comment: invalid field length");
      if (io->enc) {
        sz take = MIN(len, sizeof key);
        io->read(io->input, at, key, take);
        Fj(take, if (key[j] == '=') { split = (u32) j + 1;  break; });
        if (picture_key(key, split)) pic = picture(io, at + split, len - split, &bytes, &head);
      }
      split = symbol(c, C_SPLIT, 8, split);
      FATAL_UNLESS(split <= len, "comment: invalid key length");
      text(c, at, split, C_KEY);
      pic = (int) symbol(c, C_FORM, 1, (u32) pic);
      if (pic) image(c, at + split, len - split, bytes, head);
      else text(c, at + split, len - split, C_VALUE);
      at += len);
    text(c, at, n - at, C_TAIL);
  }
  if (c->used) io->write(io->output, c->out, c->used);
  free(c->hash);  free(c);
}
