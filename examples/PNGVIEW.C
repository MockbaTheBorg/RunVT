/* pngview.c - displays a PNG file as a sixel image on a sixel-capable
 * terminal (RunVT). Handles 8-bit RGB/RGBA, non-interlaced PNGs, with
 * a real (if compact) DEFLATE inflate - no external libraries needed.
 * Colors are quantized to a fixed 6x6x6 RGB cube (216 sixel registers).
 *
 * Never holds the whole image in memory: PNG rows stream through a
 * 32KB LZ77 window (the max DEFLATE can reference back into) and get
 * pushed out six rows (one sixel band) at a time.
 *
 * Build: 'C309-21 PNGVIEW.C'
 * Run:   pngview file.png
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXBITS 15
#define WSIZE 0x8000
#define WMASK 0x7FFF

struct huffman { short *count; short *symbol; };

/* ---- forward declarations ---- */
static int construct(struct huffman *h, unsigned char *length, int n);
static int decode(struct huffman *h);
static void build_fixed_tables(void);
static void build_dynamic_tables(void);
static int nextcompbyte(void);
static long read_be32(void);
static void read_type(char *t);
static int getbit(void);
static int getbits(int n);
static void emit(int c);
static int inflate_byte(void);
static int paeth(int a, int b, int c);
static void unfilter(int ftype, unsigned char *cur, unsigned char *prev, int n, int bpp);
static void emit_sixel_header(int width, int height);
static void emit_band(unsigned char **band, int rows, int width);
static void quantize_row(unsigned char *row, int width, int bpp, unsigned char *out);

/* ---- length/distance code tables (RFC1951) ---- */
static short length_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static unsigned char length_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static short dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static unsigned char dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* ---- huffman decode state ---- */
static short lencnt[MAXBITS+1], lensym[288];
static short distcnt[MAXBITS+1], distsym[30];
static struct huffman litcode, distcode;

/* ---- bit reader over the compressed byte stream ---- */
static int bitbuf = 0, bitcnt = 0;

/* ---- inflate block state, persists across inflate_byte() calls ---- */
static int bfinal = 0, mode = 0, done = 0;
static long stored_remaining = 0;
static int pend_len = 0;
static unsigned pend_dist = 0;

/* ---- output window, doubles as the LZ77 history ---- */
static unsigned char window[WSIZE];
static unsigned long woutpos = 0;

/* ---- PNG chunk / IDAT byte source ---- */
static FILE *fp;
static long idat_remaining = 0;

static long read_be32(void)
{
    int b0 = fgetc(fp), b1 = fgetc(fp), b2 = fgetc(fp), b3 = fgetc(fp);
    return ((long)b0 << 24) | ((long)b1 << 16) | ((long)b2 << 8) | (long)b3;
}

static void read_type(char *t)
{
    t[0] = (char)fgetc(fp); t[1] = (char)fgetc(fp);
    t[2] = (char)fgetc(fp); t[3] = (char)fgetc(fp);
    t[4] = 0;
}

static int nextcompbyte(void)
{
    char type[5];
    long len;
    int c;

    while (idat_remaining == 0) {
        fseek(fp, 4, SEEK_CUR); /* CRC of the chunk we just finished */
        len = read_be32();
        read_type(type);
        if (strcmp(type, "IDAT") == 0) {
            idat_remaining = len;
        } else {
            return -1; /* IEND or a spec violation - either way, no more data */
        }
    }
    c = fgetc(fp);
    if (c == EOF) return -1;
    idat_remaining--;
    return c;
}

static int getbit(void)
{
    int b;
    if (bitcnt == 0) {
        bitbuf = nextcompbyte();
        bitcnt = 8;
    }
    b = bitbuf & 1;
    bitbuf >>= 1;
    bitcnt--;
    return b;
}

static int getbits(int n)
{
    int v = 0, i;
    for (i = 0; i < n; i++) v |= getbit() << i;
    return v;
}

static void emit(int c)
{
    window[(unsigned)(woutpos & WMASK)] = (unsigned char)c;
    woutpos++;
}

/* canonical huffman table build from code lengths - see RFC1951 3.2.2 */
static int construct(struct huffman *h, unsigned char *length, int n)
{
    int symbol, len, left;
    short offs[MAXBITS+1];

    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;

    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }

    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) offs[len+1] = offs[len] + h->count[len];
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;

    return left;
}

static int decode(struct huffman *h)
{
    int len, code = 0, first = 0, index = 0, count;

    for (len = 1; len <= MAXBITS; len++) {
        code |= getbit();
        count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static void build_fixed_tables(void)
{
    static unsigned char lengths[288];
    int sym = 0;
    for (; sym < 144; sym++) lengths[sym] = 8;
    for (; sym < 256; sym++) lengths[sym] = 9;
    for (; sym < 280; sym++) lengths[sym] = 7;
    for (; sym < 288; sym++) lengths[sym] = 8;
    construct(&litcode, lengths, 288);
    for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
    construct(&distcode, lengths, 30);
}

static void build_dynamic_tables(void)
{
    static unsigned char lengths[320];
    static unsigned char cl_lengths[19];
    static unsigned char order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    static short clcnt[MAXBITS+1], clsym[19];
    struct huffman cltable;
    int hlit, hdist, hclen, i, sym, rep, prevlen;

    hlit = getbits(5) + 257;
    hdist = getbits(5) + 1;
    hclen = getbits(4) + 4;

    for (i = 0; i < 19; i++) cl_lengths[i] = 0;
    for (i = 0; i < hclen; i++) cl_lengths[order[i]] = (unsigned char)getbits(3);

    cltable.count = clcnt;
    cltable.symbol = clsym;
    construct(&cltable, cl_lengths, 19);

    i = 0;
    while (i < hlit + hdist) {
        sym = decode(&cltable);
        if (sym < 16) {
            lengths[i++] = (unsigned char)sym;
        } else if (sym == 16) {
            rep = getbits(2) + 3;
            prevlen = lengths[i-1];
            while (rep--) lengths[i++] = (unsigned char)prevlen;
        } else if (sym == 17) {
            rep = getbits(3) + 3;
            while (rep--) lengths[i++] = 0;
        } else {
            rep = getbits(7) + 11;
            while (rep--) lengths[i++] = 0;
        }
    }
    construct(&litcode, lengths, hlit);
    construct(&distcode, lengths + hlit, hdist);
}

/* pulls exactly one decompressed byte, running just enough of the
 * inflate state machine to produce it - never decodes ahead of what
 * the PNG unfilter loop below is ready to consume. */
static int inflate_byte(void)
{
    int sym, len, dist, btype, c;

    for (;;) {
        if (pend_len > 0) {
            c = window[(unsigned)((woutpos - pend_dist) & WMASK)];
            emit(c);
            pend_len--;
            return c;
        }
        if (done) return -1;

        if (mode == 0) {
            bfinal = getbits(1);
            btype = getbits(2);
            if (btype == 0) {
                bitcnt = 0; /* byte-align before LEN/NLEN */
                { int lo = nextcompbyte(), hi = nextcompbyte();
                  nextcompbyte(); nextcompbyte(); /* NLEN, unchecked */
                  stored_remaining = (long)lo | ((long)hi << 8); }
                mode = 1;
            } else if (btype == 1) {
                build_fixed_tables();
                mode = 2;
            } else if (btype == 2) {
                build_dynamic_tables();
                mode = 2;
            } else {
                done = 1;
                return -1;
            }
        }

        if (mode == 1) {
            if (stored_remaining > 0) {
                c = nextcompbyte();
                stored_remaining--;
                emit(c);
                if (stored_remaining == 0) {
                    mode = 0;
                    if (bfinal) done = 1;
                }
                return c;
            }
            mode = 0;
            continue;
        }

        /* mode == 2: huffman-coded block */
        sym = decode(&litcode);
        if (sym < 256) {
            emit(sym);
            return sym;
        }
        if (sym == 256) {
            mode = 0;
            if (bfinal) { done = 1; return -1; }
            continue;
        }
        sym -= 257;
        len = length_base[sym] + getbits(length_extra[sym]);
        dist = decode(&distcode);
        pend_dist = (unsigned)(dist_base[dist] + getbits(dist_extra[dist]));
        pend_len = len;
    }
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static void unfilter(int ftype, unsigned char *cur, unsigned char *prev, int n, int bpp)
{
    int x, a, b, c;
    for (x = 0; x < n; x++) {
        a = (x >= bpp) ? cur[x-bpp] : 0;
        b = prev[x];
        c = (x >= bpp) ? prev[x-bpp] : 0;
        switch (ftype) {
        case 1: cur[x] = (unsigned char)(cur[x] + a); break;
        case 2: cur[x] = (unsigned char)(cur[x] + b); break;
        case 3: cur[x] = (unsigned char)(cur[x] + (a+b)/2); break;
        case 4: cur[x] = (unsigned char)(cur[x] + paeth(a,b,c)); break;
        }
    }
}

/* fixed 6x6x6 color cube, defined once up front - see RunVT sixel.h,
 * register colors are RGB percentages (0-100), not 0-255 bytes */
static void emit_sixel_header(int width, int height)
{
    int r, g, b, reg;
    printf("\033Pq\"1;1;%d;%d", width, height);
    for (r = 0; r < 6; r++)
        for (g = 0; g < 6; g++)
            for (b = 0; b < 6; b++) {
                reg = r*36 + g*6 + b;
                printf("#%d;2;%d;%d;%d", reg, r*20, g*20, b*20);
            }
}

static void quantize_row(unsigned char *row, int width, int bpp, unsigned char *out)
{
    int x, r, g, b;
    for (x = 0; x < width; x++) {
        r = row[x*bpp+0] * 6 / 256;
        g = row[x*bpp+1] * 6 / 256;
        b = row[x*bpp+2] * 6 / 256;
        out[x] = (unsigned char)(r*36 + g*6 + b);
    }
}

/* one sixel band = up to 6 pixel rows (fewer for a short final band).
 * band[6][width] holds each pixel's color register. */
static void emit_band(unsigned char **band, int rows, int width)
{
    static unsigned char used[216];
    int reg, x, r, bits, first = 1;

    memset(used, 0, sizeof(used));
    for (r = 0; r < rows; r++)
        for (x = 0; x < width; x++)
            used[band[r][x]] = 1;

    for (reg = 0; reg < 216; reg++) {
        if (!used[reg]) continue;

        if (!first) putchar('$');
        first = 0;
        printf("#%d", reg);
        for (x = 0; x < width; x++) {
            bits = 0;
            for (r = 0; r < rows; r++)
                if (band[r][x] == reg) bits |= (1 << r);
            putchar(0x3F + bits);
        }
    }
    putchar('-');
}

main(argc, argv)
char **argv;
{
    static unsigned char sig[8] = {137,80,78,71,13,10,26,10};
    unsigned char buf[8];
    char type[5];
    long len, width, height;
    int bitdepth, colortype, compression, filtertype, interlace, bpp;
    unsigned char *currow, *prevrow, *band[6];
    int y, x, bandrow, r, ft;

    if (argc < 2) {
        printf("usage: pngview file.png\n");
        exit(1);
    }

    fp = fopen(argv[1], "rb");
    if (!fp) {
        printf("pngview: can't open %s\n", argv[1]);
        exit(1);
    }

    fread(buf, 1, 8, fp);
    if (memcmp(buf, sig, 8) != 0) {
        printf("pngview: %s is not a PNG file\n", argv[1]);
        exit(1);
    }

    len = read_be32();
    read_type(type);
    if (strcmp(type, "IHDR") != 0 || len != 13) {
        printf("pngview: bad IHDR\n");
        exit(1);
    }
    width = read_be32();
    height = read_be32();
    bitdepth = fgetc(fp);
    colortype = fgetc(fp);
    compression = fgetc(fp);
    filtertype = fgetc(fp);
    interlace = fgetc(fp);
    fseek(fp, 4, SEEK_CUR); /* IHDR CRC */

    if (bitdepth != 8 || (colortype != 2 && colortype != 6) ||
        compression != 0 || filtertype != 0 || interlace != 0) {
        printf("pngview: only plain 8-bit RGB/RGBA, non-interlaced PNGs are supported\n");
        exit(1);
    }
    if (width > 640 || height > 400) {
        printf("pngview: image too big for the screen (%ldx%ld, max 640x400)\n", width, height);
        exit(1);
    }

    for (;;) {
        len = read_be32();
        read_type(type);
        if (strcmp(type, "IDAT") == 0) { idat_remaining = len; break; }
        if (strcmp(type, "IEND") == 0) {
            printf("pngview: no image data in %s\n", argv[1]);
            exit(1);
        }
        fseek(fp, len + 4, SEEK_CUR);
    }

    nextcompbyte(); nextcompbyte(); /* zlib header (CMF, FLG) - unchecked */

    litcode.count = lencnt; litcode.symbol = lensym;
    distcode.count = distcnt; distcode.symbol = distsym;

    bpp = (colortype == 6) ? 4 : 3;
    currow = malloc((unsigned)(width * bpp));
    prevrow = malloc((unsigned)(width * bpp));
    for (r = 0; r < 6; r++) band[r] = malloc((unsigned)width);
    if (!currow || !prevrow || !band[0] || !band[1] || !band[2] ||
        !band[3] || !band[4] || !band[5]) {
        printf("pngview: out of memory\n");
        exit(1);
    }
    memset(prevrow, 0, (unsigned)(width * bpp));

    printf("\033[2J\033[H");
    emit_sixel_header((int)width, (int)height);

    bandrow = 0;
    for (y = 0; y < height; y++) {
        ft = inflate_byte();
        for (x = 0; x < width * bpp; x++) currow[x] = (unsigned char)inflate_byte();
        unfilter(ft, currow, prevrow, (int)(width * bpp), bpp);
        quantize_row(currow, (int)width, bpp, band[bandrow]);
        bandrow++;
        if (bandrow == 6) {
            emit_band(band, 6, (int)width);
            bandrow = 0;
        }
        memcpy(prevrow, currow, (unsigned)(width * bpp));
    }
    if (bandrow > 0) emit_band(band, bandrow, (int)width);

    printf("\033\\");
    fclose(fp);
    getchar();
    printf("\033[2J\033[H");
    return 0;
}
