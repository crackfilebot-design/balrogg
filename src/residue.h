/*  Copyright (C) 2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.  */

/*  RS_CM selects mixed stages; RS_ENC is 0/1 for a fixed coding direction
    or -1 for both. RS_MATCH selects the match model. Dispatch happens outside
    the partition loop.  */
#define PHASH(st_) cm_hst(phb, phx, (u32) (st_))
#define RS_ENCODING(s_) (RS_ENC < 0 ? (s_)->z->enc : RS_ENC)

#if RS_CM
static INLINE int RS_NAME(mix)(cm * c, int st, int sel, u32 h, u32 * p,
                               int exp, int bit) {
#if RS_MATCH
  return cm_bit(c, st, sel, h, p, exp, bit);
#else
  return cm_plain(c, st, sel, h, p, bit);
#endif
}
#endif

#if RS_CM != 31
static INLINE int RS_NAME(bit)(const rs_ctx * s, u32 * p, int bit) {
  if (!RS_ENCODING(s)) return rc_dec_bit_packed(s->d, p, s->lim);
  rc_enc_bit_packed(s->e, p, s->lim, bit);
  return bit;
}
#endif

/*  Keep digit temporaries out of the surrounding symbol loop. Each stage
    has a fixed model path, and plain digits need no mixer state.  */
static HOT NOINLINE i32 RS_NAME(val)(const rs_ctx * s, u32 c, u32 ch, i32 v) {
  vb_ctx * n = s->n;
  u32 * o;
  u32 ax, il, m, a, mag = 0, k, idx;
  u32 la = n->fh[0], nb = 0;
  u8 * mw;
  int t, sg = 0;
#if RS_CM
  int psel = 0, memf = 0;
  u32 pslot = 0, phb = 0, phx = 0;
  i32 pv = 0;                 /*  the match model's digit, while it holds  */
  u32 pm = 0;
  int mok = 0, mex = -1;
#endif
#ifdef BLR_PROFILE
  int e_sh = n->sh[ch], e_mh = n->mh[0];
#endif
  FATAL_IF_HOT(c >= AR_MAXIDX)
    ("vorbis: residue index %lu out of range", (unsigned long) c);
  mw = s->mw + c * AR_NCH + ch;
  m = s->mr[c * AR_NCH + ch];
  ax = n->psl == s->slot;                        /*  A_RZERO  */
  ax = ax * 2 + !(m & 7);
  ax = ax * 2 + la;
  ax = ax * AR_NBIN + c / 4;
  o = s->tab[A_RZERO - A_RZERO] + ax * AR_NCH + ch;
#if RS_CM
  /*  Mixer neighborhood uses adjacent and cross-channel digits plus class.  */
  { int c1, cx;
    if (n->nstarted && n->nidx[ch] + 1 == c) n->nrun[ch]++;
    else n->nrun[ch] = 0;
    c1 = n->nrun[ch] >= 1 ? mcls(n->nv0[ch]) : 0;

    /*  For interleaved residue, cx is the other channel at this bin.  */
    cx = n->nstarted && (u32) n->npch != ch ? mcls(n->nxv) : 0;
    memf = (int) ((m & 3) + 4 * (m >> 7));
    psel = ((c1 * 5 + cx) * 2 + (int) (ch & 1)) * 4 + (int) (m & 3);
    pslot = s->slot;
    phb = cm_hpre(pslot, ch, c, (u32) memf);
    phx = cm_hpx(pslot, ch, c, (u32) memf);
    if (RS_MATCH) mok = cm_match(&n->cm, &pv);
    if (mok) pm = (u32) (pv < 0 ? -pv : pv);
  }
#endif
  PROF(prof_site = P_RZERO);
  /*  Each stage expects the predicted digit's bit until one disagrees.  */
#if RS_CM & 1
  mex = mok ? pv != 0 : -1;
  t = RS_NAME(mix)(&n->cm, 0, psel, PHASH(0), o, mex, v != 0);
#else
  t = RS_NAME(bit)(s, o, v != 0);
#endif
#if RS_CM
  if (t != (pv != 0)) mok = 0;
#endif
  n->psl = s->slot;
  if (!t) {
    n->fh[ch] = 0;  n->mh[ch] = 0;  *mw = 0;
#if RS_CM
    if (RS_MATCH) cm_match_push(&n->cm, 0);
#endif
    PROF(prof_res((int) s->slot, (int) s->q, (int) s->pass, (int) ch, c, (int) m,
                  (int) la, e_sh, e_mh, 0));
    PROF(prof_site = P_VOTHER);
#if RS_CM
    cm_step(n, ch, c, 0);
#endif
    return 0;
  }
  il = blr_ilog(c);  n->fh[ch] = 1;
  if (RS_ENCODING(s)) mag = (u32) (v < 0 ? -v : v);
  ax = m != 0;                                  /*  A_RSIGN  */
  ax = ax * 2 + (m >> 7 & 1);
  ax = ax * 2 + la;
  ax = ax * AR_HIST2 + n->sh[ch];
  ax = ax * AR_ILOG + il;
  o = s->tab[A_RSIGN - A_RZERO] + ax * AR_NCH + ch;
  PROF(prof_site = P_RSIGN);
#if RS_CM & 2
  mex = mok ? pv < 0 : -1;
  sg = RS_NAME(mix)(&n->cm, 1, psel, PHASH(1), o, mex, v < 0);
#else
  sg = RS_NAME(bit)(s, o, v < 0);
#endif
#if RS_CM
  if (sg != (pv < 0)) mok = 0;
#endif
  ax = m != 0;                                  /*  A_RONE  */
  ax = ax * AR_MCLS + n->mh[0];
  ax = ax * 2 + (u32) sg;
  ax = ax * AR_ILOG + il;
  o = s->tab[A_RONE - A_RZERO] + ax * AR_NCH + ch;
  PROF(prof_site = P_RONE);
#if RS_CM & 4
  mex = mok ? pm == 1 : -1;
  t = RS_NAME(mix)(&n->cm, 2, psel, PHASH(2), o, mex, mag == 1);
#else
  t = RS_NAME(bit)(s, o, mag == 1);
#endif
#if RS_CM
  if (t != (pm == 1)) mok = 0;
#endif
  n->sh[ch] = (u8) ((sg + n->sh[ch] * 2) & 3);
  if (t) { n->mh[ch] = 1;  *mw = (u8) ((sg << 7) + 1);  mag = 1; }
  else {
#if RS_CM & 24
    u32 pnb = mok ? blr_ilog(pm) - 2 : 0;
#endif
#if RS_CM & 8
    u32 h3;
#endif
#if RS_CM & 16
    u32 h4;
#endif
    if (RS_ENCODING(s)) nb = blr_ilog(mag) - 2;
    FATAL_IF_HOT(RS_ENCODING(s) && !(mag >= 2 && nb < AR_MAGB))
      ("vorbis: residue digit %ld exceeds 8 bits", (long) v);
    ax = n->mh[0];                              /*  A_RLEN  */
    o = s->tab[A_RLEN - A_RZERO] + (ax * AR_NCH + ch) * 8;
    PROF(prof_site = P_RLEN);
    idx = 1;
#if RS_CM & 8
    h3 = PHASH(3);
#endif
    for (k = 3; k > 0; k--) {
#if RS_CM & 24
      mex = mok ? (int) (pnb >> (k - 1) & 1) : -1;
#endif
#if RS_CM & 8
      t = RS_NAME(mix)(&n->cm, 3, psel, h3, o + idx, mex, (int) (nb >> (k - 1) & 1));
#else
      t = RS_NAME(bit)(s, o + idx, (int) (nb >> (k - 1) & 1));
#endif
#if RS_CM & 24
      if (t != mex) mok = 0;
#endif
      idx = idx * 2 + (u32) t;
    }
    nb = idx - 8;
    ax = nb;                                   /*  A_RMANT  */
    ax = ax * AR_MCLS + n->mh[0];
    o = s->tab[A_RMANT - A_RZERO] + (ax * AR_NCH + ch) * AR_LOW2;
    PROF(prof_site = P_RMANT);
    a = 1;
#if RS_CM & 16
    h4 = PHASH(4);
#endif
    for (k = nb + 1; k > 0; k--) {
#if RS_CM & 16
      mex = mok ? (int) (pm >> (k - 1) & 1) : -1;
      t = RS_NAME(mix)(&n->cm, 4, psel, h4, o + (a & 3), mex,
                 (int) (mag >> (k - 1) & 1));
      if (t != mex) mok = 0;
#else
      t = RS_NAME(bit)(s, o + (a & 3), (int) (mag >> (k - 1) & 1));
#endif
      a = a * 2 + (u32) t;
    }
    mag = a;  n->mh[ch] = (u8) (nb + 2);  *mw = (u8) ((sg << 7) + 2 + nb);
  }
  { i32 rv = sg ? -(i32) mag : (i32) mag;
    PROF(prof_res((int) s->slot, (int) s->q, (int) s->pass, (int) ch, c, (int) m,
                  (int) la, e_sh, e_mh, rv));
    PROF(prof_site = P_VOTHER);
#if RS_CM
    cm_step(n, ch, c, rv);
    if (RS_MATCH) cm_match_push(&n->cm, rv);
#endif
    return rv; }
}

/*  Process one codebook symbol as base-`nv` digits.  */
static INLINE void RS_NAME(sym)(const rs_ctx * s, vb_book * b, u32 g, u32 st,
                               u32 il) {
  io * z = s->z;
  u32 k, e = 0, np = 1, t = 0, c, ch;
  i32 d;
  if (RS_ENCODING(s)) { e = bk_get(z, b);  t = e; }
  if (z->probe) return;
  if (il == 2) { c = g >> 1;  ch = g & 1; }
  else if (il > 2) { c = g / il;  ch = g % il; }
  else { c = g;  ch = 0; }
  Fk(b->dim,
    u32 chc = ch > 3 ? 3 : ch;
    if (RS_ENCODING(s)) {
      u32 next = (u32) ((uint64_t) t * b->divmul >> b->divshift);
      d = (i32) b->mult[t - next * b->nv] - (i32) b->off;  t = next;
      RS_NAME(val)(s, c, chc, d);
    } else {
      u32 p;
      d = RS_NAME(val)(s, c, chc, 0);
      FATAL_IF_HOT(!(d + (i32) b->off >= 0 && d + (i32) b->off < (i32) b->base))
        ("vorbis: residue digit %ld outside codebook grid", (long) d);
      p = b->inv[(u32) (d + (i32) b->off)];
      FATAL_IF_HOT(p == (u32) -1)
        ("vorbis: residue digit %ld is not a codebook multiplicand", (long) d);
      /*  Digit k weighs nv^k, the same decomposition the encoder took the
          entry apart with.  */
      e += p * np;  np *= b->nv;
    }
    if (il) { ch += st;  while (ch >= il) { ch -= il;  c++; } }
    else c += st);
  if (!RS_ENCODING(s)) {
    FATAL_IF_HOT(e >= b->ent)("vorbis: residue has no codebook entry");
    bk_put(z, b, e);
  }
}

/*  Process one partition with inlined symbol operations.  */
static HOT FLATTEN void RS_NAME(part)(io * z, vb_res * r, vb_book * b, u32 q,
                                     u32 pass, u32 g, u32 il) {
  rs_ctx s;
  u32 i, st;
  rs_init(&s, z, b->slot, q, pass);
  if (r->type == 0) {
    st = r->psz / b->dim;
    FATAL_IF_HOT(st * b->dim != r->psz)
      ("vorbis: residue 0 partition %lu not divisible by %lu",
       (unsigned long) r->psz, (unsigned long) b->dim);
    Fi(st, RS_NAME(sym)(&s, b, g + i, st, il);
           if (z->rawpkt) return);
  } else
    for (i = 0; i < r->psz && !z->rawpkt; i += b->dim)
      RS_NAME(sym)(&s, b, g + i, 1, il);
}

#undef RS_MATCH
#undef RS_ENCODING
#undef RS_ENC
#undef PHASH
#undef RS_NAME
#undef RS_CM
