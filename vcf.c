#include <zlib.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "kstring.h"
#include "bgzf.h"
#include "vcf.h"

#include "khash.h"
KHASH_MAP_INIT_STR(vdict, vcf_idinfo_t)
typedef khash_t(vdict) vdict_t;

#include "kseq.h"
KSTREAM_INIT(gzFile, gzread, 16384)

int vcf_verbose = 3; // 1: error; 2: warning; 3: message; 4: progress; 5: debugging; >=10: pure debugging
uint32_t vcf_missing_float = 0x7F800001;
uint8_t vcf_type_shift[] = { 0, 0, 1, 2, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static vcf_idinfo_t vcf_idinfo_def = { { 15, 15, 15 }, -1 };

/*************
 * Basic I/O *
 *************/

vcfFile *vcf_open(const char *fn, const char *mode, const char *fn_ref)
{
	const char *p;
	vcfFile *fp;
	fp = (vcfFile*)calloc(1, sizeof(vcfFile));
	for (p = mode; *p; ++p) {
		if (*p == 'w') fp->is_write = 1;
		else if (*p == 'b') fp->is_bin = 1;
	}
	if (fp->is_bin) {
		if (fp->is_write) fp->fp = strcmp(fn, "-")? bgzf_open(fn, mode) : bgzf_dopen(fileno(stdout), mode);
		else fp->fp = strcmp(fn, "-")? bgzf_open(fn, "r") : bgzf_dopen(fileno(stdin), "r");
	} else {
		if (fp->is_write) {
			fp->fp = strcmp(fn, "-")? fopen(fn, "rb") : stdout;
		} else {
			gzFile gzfp;
			gzfp = strcmp(fn, "-")? gzopen(fn, "rb") : gzdopen(fileno(stdin), "rb");
			if (gzfp) fp->fp = ks_init(gzfp);
			if (fn_ref) fp->fn_ref = strdup(fn_ref);
		}
	}
	if (fp->fp == 0) {
		if (vcf_verbose >= 2)
			fprintf(stderr, "[E::%s] fail to open file '%s'\n", __func__, fn);
		free(fp->fn_ref); free(fp);
		return 0;
	}
	return fp;
}

void vcf_close(vcfFile *fp)
{
	if (!fp->is_bin) {
		free(fp->line.s);
		if (!fp->is_write) {
			gzFile gzfp = ((kstream_t*)fp->fp)->f;
			ks_destroy((kstream_t*)fp->fp);
			gzclose(gzfp);
			free(fp->fn_ref);
		} else fclose((FILE*)fp->fp);
	} else bgzf_close((BGZF*)fp->fp);
	free(fp);
}

/*********************
 * VCF header parser *
 *********************/

// return: positive => contig; zero => INFO/FILTER/FORMAT; negative => error or skipped
int vcf_hdr_parse_line2(const char *str, uint32_t *info, int *id_beg, int *id_end)
{
	const char *p, *q;
	int ctype;
	int type = -1; // Type
	int num = -1; // Number
	int var = -1; // A, G, ., or fixed
	int ctg_len = -1;

	if (*str != '#' && str[1] != '#') return -1;
	*id_beg = *id_end = *info = -1;
	p = str + 2;
	for (q = p; *q && *q != '='; ++q); // FIXME: do we need to check spaces?
	if (*q == 0) return -2;
	if (q - p == 4 && strncmp(p, "INFO", 4) == 0) ctype = VCF_HL_INFO;
	else if (q - p == 6 && strncmp(p, "FILTER", 6) == 0) ctype = VCF_HL_FLT;
	else if (q - p == 6 && strncmp(p, "FORMAT", 6) == 0) ctype = VCF_HL_FMT;
	else if (q - p == 6 && strncmp(p, "contig", 6) == 0) ctype = VCF_HL_CTG;
	else return -3;
	for (; *q && *q != '<'; ++q);
	if (*q == 0) return -3;
	p = q + 1; // now p points to the first character following '<'
	while (*p && *p != '>') {
		int which = 0;
		char *tmp;
		const char *val;
		for (q = p; *q && *q != '='; ++q);
		if (*q == 0) break;
		if (q - p == 2 && strncmp(p, "ID", 2) == 0) which = 1; // ID
		else if (q - p == 4 && strncmp(p, "Type", 4) == 0) which = 2; // Number
		else if (q - p == 6 && strncmp(p, "Number", 6) == 0) which = 3; // Type
		else if (q - p == 6 && strncmp(p, "length", 6) == 0) which = 4; // length
		val = q + 1;
		if (*val == '"') { // quoted string
			for (q = val + 1; *q && *q != '"'; ++q)
				if (*q == '\\' && *(q+1) != 0) ++q;
			if (*q != '"') return -4; // open double quotation mark
			p = q + 1;
			if (*p == ',') ++p;
			continue;
		}
		for (q = val; *q && *q != ',' && *q != '>'; ++q); // parse val
		if (which == 1) {
			*id_beg = val - str; *id_end = q - str;
		} else if (which == 2) {
			if (q - val == 7 && strncmp(val, "Integer", 7) == 0) type = VCF_HT_INT;
			else if (q - val == 5 && strncmp(val, "Float", 5) == 0) type = VCF_HT_REAL;
			else if (q - val == 6 && strncmp(val, "String", 6) == 0) type = VCF_HT_STR;
			else if (q - val == 4 && strncmp(val, "Flag", 6) == 0) type = VCF_HT_FLAG;
		} else if (which == 3) {
			if (*val == 'A') var = VCF_VL_A;
			else if (*val == 'G') var = VCF_VL_G;
			else if (isdigit(*val)) var = VCF_VL_FIXED, num = strtol(val, &tmp, 10);
			else var = VCF_VL_VAR;
			if (var != VCF_VL_FIXED) num = 0xfffff;
		} else if (which == 4) {
			if (isdigit(*val)) ctg_len = strtol(val, &tmp, 10);
		}
		p = q + 1;
	}
	if (ctype == VCF_HL_CTG) {
		if (ctg_len > 0) return ctg_len;
		else return -5;
	} else {
		if (ctype == VCF_HL_FLT) num = 0;
		if (type == VCF_HT_FLAG) {
			if (num != 0 && vcf_verbose >= 2)
				fprintf(stderr, "[W::%s] ignore Number for a Flag\n", __func__);
			num = 0, var = VCF_VL_FIXED; // if Flag VCF type, force to change num to 0
		}
		if (num == 0) type = VCF_HT_FLAG, var = VCF_VL_FIXED; // conversely, if num==0, force the type to Flag
		if (*id_beg < 0 || type < 0 || num < 0 || var < 0) return -5; // missing information
		*info = (uint32_t)num<<12 | var<<8 | type<<4 | ctype;
		//printf("%d, %s, %d, %d, [%d,%d]\n", ctype, vcf_type_name[type], var, num, *id_beg, *id_end);
		return 0;
	}
}

vcf_hdr_t *vcf_hdr_init(void)
{
	int i;
	vcf_hdr_t *h;
	h = (vcf_hdr_t*)calloc(1, sizeof(vcf_hdr_t));
	for (i = 0; i < 3; ++i)
		h->dict[i] = kh_init(vdict);
	return h;
}

void vcf_hdr_destroy(vcf_hdr_t *h)
{
	int i;
	khint_t k;
	for (i = 0; i < 3; ++i) {
		vdict_t *d = (vdict_t*)h->dict[i];
		for (k = kh_begin(d); k != kh_end(d); ++k)
			if (kh_exist(d, k)) free((char*)kh_key(d, k));
		kh_destroy(vdict, d);
		free(h->id[i]);
	}
	free(h->mem.s); free(h->text);
	free(h);
}

int vcf_hdr_parse1(vcf_hdr_t *h, const char *str)
{
	khint_t k;
	if (*str != '#') return -1;
	if (str[1] == '#') {
		uint32_t info;
		int len, ret, id_beg, id_end;
		char *s;

		len = vcf_hdr_parse_line2(str, &info, &id_beg, &id_end);
		if (len < 0) return -1;
		s = (char*)malloc(id_end - id_beg + 1);
		strncpy(s, str + id_beg, id_end - id_beg);
		s[id_end - id_beg] = 0;
		if (len > 0) { // a contig line
			vdict_t *d = (vdict_t*)h->dict[VCF_DT_CTG];
			k = kh_put(vdict, d, s, &ret);
			if (ret == 0) {
				if (vcf_verbose >= 2)
					fprintf(stderr, "[W::%s] Duplicated contig name '%s'. Skipped.\n", __func__, s);
				free(s);
			} else {
				kh_val(d, k) = vcf_idinfo_def;
				kh_val(d, k).id = kh_size(d) - 1;
				kh_val(d, k).info[0] = len;
			}
		} else { // a FILTER/INFO/FORMAT line
			vdict_t *d = (vdict_t*)h->dict[VCF_DT_ID];
			k = kh_put(vdict, d, s, &ret);
			if (ret) { // absent from the dict
				kh_val(d, k) = vcf_idinfo_def;
				kh_val(d, k).info[info&0xf] = info;
				kh_val(d, k).id = kh_size(d) - 1;
			} else {
				kh_val(d, k).info[info&0xf] = info;
				free(s);
			}
		}
	} else {
		int i = 0;
		const char *p, *q;
		vdict_t *d = (vdict_t*)h->dict[VCF_DT_ID];
		// check if "PASS" is in the dictionary
		k = kh_get(vdict, d, "PASS");
		if (k == kh_end(d)) vcf_hdr_parse1(h, "##FILTER=<ID=PASS>"); // if not, add it; this is a recursion
		// add samples
		d = (vdict_t*)h->dict[VCF_DT_SAMPLE];
		for (p = q = str;; ++q) {
			int ret;
			if (*q != '\t' && *q != 0) continue;
			if (++i > 9) {
				char *s;
				s = (char*)malloc(q - p + 1);
				strncpy(s, p, q - p);
				s[q - p] = 0;
				k = kh_put(vdict, d, s, &ret);
				if (ret) { // absent
					kh_val(d, k) = vcf_idinfo_def;
					kh_val(d, k).id = kh_size(d) - 1;
				} else {
					if (vcf_verbose >= 2)
						fprintf(stderr, "[W::%s] Duplicated sample name '%s'. Skipped.\n", __func__, s);
				}
			}
			if (*q == 0) break;
			p = q + 1;
		}
	}
	return 0;
}

int vcf_hdr_sync(vcf_hdr_t *h)
{
	int i;
	for (i = 0; i < 3; ++i) {
		khint_t k;
		vdict_t *d = (vdict_t*)h->dict[i];
		h->n[i]  = kh_size(d);
		h->id[i] = (vcf_idpair_t*)malloc(kh_size(d) * sizeof(vcf_idpair_t));
		for (k = kh_begin(d); k != kh_end(d); ++k) {
			if (!kh_exist(d, k)) continue;
			h->id[i][kh_val(d, k).id].key = kh_key(d, k);
			h->id[i][kh_val(d, k).id].val = &kh_val(d, k);
		}
	}
	return 0;
}

int vcf_hdr_parse(vcf_hdr_t *h)
{
	char *p, *q;
	for (p = q = h->text;; ++q) {
		int c;
		if (*q != '\n' && *q != 0) continue;
		c = *q; *q = 0;
		vcf_hdr_parse1(h, p);
		*q = c;
		if (*q == 0) break;
		p = q + 1;
	}
	vcf_hdr_sync(h);
	return 0;
}

/******************
 * VCF header I/O *
 ******************/

vcf_hdr_t *vcf_hdr_read(vcfFile *fp)
{
	vcf_hdr_t *h;
	if (fp->is_write) return 0;
	h = vcf_hdr_init();
	if (fp->is_bin) {
		uint8_t magic[4];
		bgzf_read((BGZF*)fp->fp, magic, 4);
		bgzf_read((BGZF*)fp->fp, &h->l_text, 4);
		h->text = (char*)malloc(h->l_text);
		bgzf_read((BGZF*)fp->fp, h->text, h->l_text);
	} else {
		int dret;
		kstring_t txt, *s = &fp->line;
		txt.l = txt.m = 0; txt.s = 0;
		while (ks_getuntil((kstream_t*)fp->fp, KS_SEP_LINE, s, &dret) >= 0) {
			if (s->l == 0) continue;
			if (s->s[0] != '#') {
				if (vcf_verbose >= 2)
					fprintf(stderr, "[E::%s] no sample line\n", __func__);
				free(txt.s);
				vcf_hdr_destroy(h);
				return 0;
			}
			if (s->s[1] != '#' && fp->fn_ref) { // insert contigs here
				gzFile f;
				kstream_t *ks;
				kstring_t tmp;
				tmp.l = tmp.m = 0; tmp.s = 0;
				f = gzopen(fp->fn_ref, "r");
				ks = ks_init(f);
				while (ks_getuntil(ks, 0, &tmp, &dret) >= 0) {
					int c;
					kputs("##contig=<ID=", &txt); kputs(tmp.s, &txt);
					ks_getuntil(ks, 0, &tmp, &dret);
					kputs(",length=", &txt); kputw(atol(tmp.s), &txt);
					kputsn(">\n", 2, &txt);
					if (dret != '\n')
						while ((c = ks_getc(ks)) != '\n' && c != -1); // skip the rest of the line
				}
				free(tmp.s);
				ks_destroy(ks);
				gzclose(f);
			}
			kputsn(s->s, s->l, &txt);
			if (s->s[1] != '#') break;
			kputc('\n', &txt);
		}
		h->l_text = txt.l + 1; // including NULL
		h->text = txt.s;
	}
	if (vcf_hdr_parse(h) != 0) { // FIXME: vcf_hdr_parse() always returns 0
		vcf_hdr_destroy(h);
		return 0;
	} else return h;
}

void vcf_hdr_write(vcfFile *fp, const vcf_hdr_t *h)
{
	if (fp->is_bin) {
		bgzf_write((BGZF*)fp->fp, "BCF\2", 4);
		bgzf_write((BGZF*)fp->fp, &h->l_text, 4);
		bgzf_write((BGZF*)fp->fp, h->text, h->l_text);
	} else {
		fwrite(h->text, 1, h->l_text, (FILE*)fp->fp);
		fputc('\n', (FILE*)fp->fp);
	}
}

/*******************
 * Typed value I/O *
 *******************/

void vcf_enc_vint(kstring_t *s, int n, int32_t *a, int wsize)
{
	int32_t max = INT32_MIN + 1, min = INT32_MAX;
	int i;
	if (n == 0) vcf_enc_size(s, 0, VCF_BT_NULL);
	else if (n == 1) vcf_enc_int1(s, a[0]);
	else {
		if (wsize <= 0) wsize = n;
		for (i = 0; i < n; ++i) {
			if (a[i] == INT32_MIN) continue;
			if (max < a[i]) max = a[i];
			if (min > a[i]) min = a[i];
		}
		if (max <= INT8_MAX && min > INT8_MIN) {
			vcf_enc_size(s, wsize, VCF_BT_INT8);
			for (i = 0; i < n; ++i)
				kputc(a[i] == INT32_MIN? INT8_MIN : a[i], s);
		} else if (max <= INT16_MAX && min > INT16_MIN) {
			vcf_enc_size(s, wsize, VCF_BT_INT16);
			for (i = 0; i < n; ++i) {
				int16_t x = a[i] == INT32_MIN? INT16_MIN : a[i];
				kputsn((char*)&x, 2, s);
			}
		} else {
			vcf_enc_size(s, wsize, VCF_BT_INT32);
			for (i = 0; i < n; ++i) {
				int32_t x = a[i] == INT32_MIN? INT32_MIN : a[i];
				kputsn((char*)&x, 4, s);
			}
		}
	}
}

void vcf_enc_vfloat(kstring_t *s, int n, float *a)
{
	vcf_enc_size(s, n, VCF_BT_FLOAT);
	kputsn((char*)a, n << 2, s);
}

void vcf_enc_vchar(kstring_t *s, int l, char *a)
{
	vcf_enc_size(s, l, VCF_BT_CHAR);
	kputsn(a, l, s);
}

void vcf_fmt_array(kstring_t *s, int n, int type, void *data)
{
	int j = 0;
	if (type == VCF_BT_INT8) {
		int8_t *p = (int8_t*)data;
		for (j = 0; j < n && *p != INT8_MIN; ++j, ++p) {
			if (j) kputc(',', s);
			kputw(*p, s);
		}
	} else if (type == VCF_BT_CHAR) {
		char *p = (char*)data;
		for (j = 0; j < n && *p; ++j, ++p) kputc(*p, s);
	} else if (type == VCF_BT_INT32) {
		int32_t *p = (int32_t*)data;
		for (j = 0; j < n && *p != INT32_MIN; ++j, ++p) {
			if (j) kputc(',', s);
			kputw(*p, s);
		}
	} else if (type == VCF_BT_FLOAT) {
		float *p = (float*)data;
		for (j = 0; j < n && *(int32_t*)p != 0x7F800001; ++j, ++p) {
			if (j) kputc(',', s);
			ksprintf(s, "%g", *p);
		}
	} else if (type == VCF_BT_INT16) {
		int16_t *p = (int16_t*)data;
		for (j = 0; j < n && *p != INT16_MIN; ++j, ++p) {
			if (j) kputc(',', s);
			kputw(*p, s);
		}
	}
	if (n && j == 0) kputc('.', s);
}

uint8_t *vcf_fmt_sized_array(kstring_t *s, uint8_t *ptr)
{
	int x, type;
	x = vcf_dec_size(ptr, &ptr, &type);
	vcf_fmt_array(s, x, type, ptr);
	return ptr + (x << vcf_type_shift[type]);
}

/****************************
 * Parsing VCF record lines *
 ****************************/

vcf1_t *vcf_init1()
{
	vcf1_t *v;
	v = (vcf1_t*)calloc(1, sizeof(vcf1_t));
	return v;
}

void vcf_destroy1(vcf1_t *v)
{
	free(v->shared.s); free(v->indiv.s);
	free(v);
}

typedef struct {
	int key, max_m, size, offset;
	uint32_t is_gt:1, max_g:15, max_l:16;
	uint32_t y;
	uint8_t *buf;
} fmt_aux_t;

static inline void align_mem(kstring_t *s)
{
	if (s->l&7) {
		uint64_t zero = 0;
		int l = ((s->l + 7)>>3<<3) - s->l;
		kputsn((char*)&zero, l, s);
	}
}

int vcf_parse1(kstring_t *s, const vcf_hdr_t *h, vcf1_t *v)
{
	int i = 0;
	char *p, *q, *r, *t;
	fmt_aux_t *fmt = 0;
	kstring_t *str, *mem = (kstring_t*)&h->mem;
	khint_t k;
	ks_tokaux_t aux;

	mem->l = v->shared.l = v->indiv.l = 0;
	str = &v->shared;
	v->n_fmt = 0;
	for (p = kstrtok(s->s, "\t", &aux), i = 0; p; p = kstrtok(0, 0, &aux), ++i) {
		q = (char*)aux.p;
		*q = 0;
		if (i == 0) { // CHROM
			vdict_t *d = (vdict_t*)h->dict[VCF_DT_CTG];
			k = kh_get(vdict, d, p);
			if (k == kh_end(d)) {
				if (vcf_verbose >= 2)
					fprintf(stderr, "[W::%s] can't find '%s' in the sequence dictionary\n", __func__, p);
				return 0;
			} else v->rid = kh_val(d, k).id;
		} else if (i == 1) { // POS
			v->pos = atoi(p) - 1;
		} else if (i == 2) { // ID
			if (strcmp(p, ".")) vcf_enc_vchar(str, q - p, p);
			else vcf_enc_size(str, 0, VCF_BT_CHAR);
		} else if (i == 3) { // REF
            if ( q-p>32767 )
            {
                fprintf(stderr, "[W::%s] The REF too long (%ld), skipping %s:%d\n", __func__, q-p, h->id[VCF_DT_CTG][v->rid].key,v->pos+1);
                return 0;
            }
			vcf_enc_vchar(str, q - p, p);
			v->n_allele = 1, v->rlen = q - p;
		} else if (i == 4) { // ALT
			if (strcmp(p, ".")) {
				for (r = t = p;; ++r) {
					if (*r == ',' || *r == 0) {
						vcf_enc_vchar(str, r - t, t);
						t = r + 1;
						++v->n_allele;
					}
					if (r == q) break;
				}
			}
		} else if (i == 5) { // QUAL
			if (strcmp(p, ".")) v->qual = atof(p);
			else memcpy(&v->qual, &vcf_missing_float, 4);
		} else if (i == 6) { // FILTER
			if (strcmp(p, ".")) {
				int32_t *a;
				int n_flt = 1, i;
				ks_tokaux_t aux1;
				vdict_t *d = (vdict_t*)h->dict[VCF_DT_ID];
				// count the number of filters
				if (*(q-1) == ';') *(q-1) = 0;
				for (r = p; *r; ++r)
					if (*r == ';') ++n_flt;
				a = (int32_t*)alloca(n_flt * 4);
				// add filters
				for (t = kstrtok(p, ";", &aux1), i = 0; t; t = kstrtok(0, 0, &aux1)) {
					*(char*)aux1.p = 0;
					k = kh_get(vdict, d, t);
					if (k == kh_end(d)) { // not defined
						if (vcf_verbose >= 2) fprintf(stderr, "[W::%s] undefined FILTER '%s'\n", __func__, t);
					} else a[i++] = kh_val(d, k).id;
				}
				n_flt = i;
				vcf_enc_vint(str, n_flt, a, -1);
			} else vcf_enc_vint(str, 0, 0, -1);
		} else if (i == 7) { // INFO
			char *key;
			vdict_t *d = (vdict_t*)h->dict[VCF_DT_ID];
			v->n_info = 0;
			if (strcmp(p, ".")) {
				if (*(q-1) == ';') *(q-1) = 0;
				for (r = key = p;; ++r) {
					int c;
					char *val, *end;
					if (*r != ';' && *r != '=' && *r != 0) continue;
					val = end = 0;
					c = *r; *r = 0;
					if (c == '=') {
						val = r + 1;
						for (end = val; *end != ';' && *end != 0; ++end);
						c = *end; *end = 0;
					} else end = r;
					k = kh_get(vdict, d, key);
					if (k == kh_end(d) || kh_val(d, k).info[VCF_HL_INFO] == 15) { // not defined in the header
						if (vcf_verbose >= 2) fprintf(stderr, "[W::%s] undefined INFO '%s'\n", __func__, key);
					} else { // defined in the header
						uint32_t y = kh_val(d, k).info[VCF_HL_INFO];
						++v->n_info;
						vcf_enc_int1(str, kh_val(d, k).id);
						if (val == 0) {
							vcf_enc_size(str, 0, VCF_BT_NULL);
						} else if ((y>>4&0xf) == VCF_HT_FLAG || (y>>4&0xf) == VCF_HT_STR) { // if Flag has a value, treat it as a string
							vcf_enc_vchar(str, end - val, val);
						} else { // int/float value/array
							int i, n_val;
							char *t;
							for (t = val, n_val = 1; *t; ++t) // count the number of values
								if (*t == ',') ++n_val;
							if ((y>>4&0xf) == VCF_HT_INT) {
								int32_t *z;
								z = (int32_t*)alloca(n_val<<2);
								for (i = 0, t = val; i < n_val; ++i, ++t)
									z[i] = strtol(t, &t, 10);
								vcf_enc_vint(str, n_val, z, -1);
								if (strcmp(key, "END") == 0) v->rlen = z[0] - v->pos;
							} else if ((y>>4&0xf) == VCF_HT_REAL) {
								float *z;
								z = (float*)alloca(n_val<<2);
								for (i = 0, t = val; i < n_val; ++i, ++t)
									z[i] = strtod(t, &t);
								vcf_enc_vfloat(str, n_val, z);
							}
						}
					}
					if (c == 0) break;
					r = end;
					key = r + 1;
				}
			}
		} else if (i == 8) { // FORMAT
			int j, l, m, g;
			ks_tokaux_t aux1;
			vdict_t *d = (vdict_t*)h->dict[VCF_DT_ID];
			char *end = s->s + s->l;
			// count the number of format fields
			for (r = p, v->n_fmt = 1; *r; ++r)
				if (*r == ':') ++v->n_fmt;
			fmt = (fmt_aux_t*)alloca(v->n_fmt * sizeof(fmt_aux_t));
			// get format information from the dictionary
			for (j = 0, t = kstrtok(p, ":", &aux1); t; t = kstrtok(0, 0, &aux1), ++j) {
				*(char*)aux1.p = 0;
				k = kh_get(vdict, d, t);
				if (k == kh_end(d) || kh_val(d, k).info[VCF_HL_FMT] == 15) {
					if (vcf_verbose >= 2)
						fprintf(stderr, "[W::%s] FORMAT '%s' is not defined in the header\n", __func__, t);
					v->n_fmt = 0;
					break;
				} else {
					fmt[j].max_l = fmt[j].max_m = fmt[j].max_g = 0;
					fmt[j].key = kh_val(d, k).id;
					fmt[j].is_gt = !strcmp(t, "GT");
					fmt[j].y = h->id[0][fmt[j].key].val->info[VCF_HL_FMT];
				}
			}
			// compute max
			for (r = q + 1, j = 0, m = l = g = 1, v->n_sample = 0;; ++r, ++l) {
				if (*r == '\t') *r = 0;
				if (*r == ':' || *r == '\0') { // end of a sample
					if (fmt[j].max_m < m) fmt[j].max_m = m;
					if (fmt[j].max_l < l - 1) fmt[j].max_l = l - 1;
					if (fmt[j].is_gt && fmt[j].max_g < g) fmt[j].max_g = g;
					l = 0, m = g = 1;
					if (*r) ++j;
					else j = 0, ++v->n_sample;
				} else if (*r == ',') ++m;
				else if (*r == '|' || *r == '/') ++g;
				if (r == end) break;
			}
			// allocate memory for arrays
			for (j = 0; j < v->n_fmt; ++j) {
				fmt_aux_t *f = &fmt[j];
				if ((f->y>>4&0xf) == VCF_HT_STR) {
					f->size = f->is_gt? f->max_g << 2 : f->max_l;
				} else if ((f->y>>4&0xf) == VCF_HT_REAL || (f->y>>4&0xf) == VCF_HT_INT) {
					f->size = f->max_m << 2;
				} else abort(); // I do not know how to do with Flag in the genotype fields
				align_mem(mem);
				f->offset = mem->l;
				ks_resize(mem, mem->l + v->n_sample * f->size);
				mem->l += v->n_sample * f->size;
			}
			for (j = 0; j < v->n_fmt; ++j)
				fmt[j].buf = (uint8_t*)mem->s + fmt[j].offset;
			// fill the sample fields; at beginning of the loop, t points to the first char of a format
			for (t = q + 1, j = m = 0;; ++t) { // j: fmt id, m: sample id
				fmt_aux_t *z = &fmt[j];
				if ((z->y>>4&0xf) == VCF_HT_STR) {
					if (z->is_gt) { // genotypes
						int32_t is_phased = 0, *x = (int32_t*)(z->buf + z->size * m);
						for (l = 0;; ++t) {
							if (*t == '.') ++t, x[l++] = is_phased;
							else x[l++] = (strtol(t, &t, 10) + 1) << 1 | is_phased;
							is_phased = (*t == '|');
							if (*t == ':' || *t == 0) break;
						}
						for (; l != z->size>>2; ++l) x[l] = INT32_MIN;
					} else {
						char *x = (char*)z->buf + z->size * m;
						for (r = t, l = 0; *t != ':' && *t; ++t) x[l++] = *t;
						for (; l != z->size; ++l) x[l] = 0;
					}
				} else if ((z->y>>4&0xf) == VCF_HT_INT) {
					int32_t *x = (int32_t*)(z->buf + z->size * m);
					for (l = 0;; ++t) {
						if (*t == '.') x[l++] = INT32_MIN, ++t; // ++t to skip "."
						else x[l++] = strtol(t, &t, 10);
						if (*t == ':' || *t == 0) break;
					}
					for (; l != z->size>>2; ++l) x[l] = INT32_MIN;
				} else if ((z->y>>4&0xf) == VCF_HT_REAL) {
					float *x = (float*)(z->buf + z->size * m);
					for (l = 0;; ++t) {
						if (*t == '.' && !isdigit(t[1])) *(int32_t*)&x[l++] = 0x7F800001, ++t; // ++t to skip "."
						else x[l++] = strtod(t, &t);
						if (*t == ':' || *t == 0) break;
					}
					for (; l != z->size>>2; ++l) *(int32_t*)(x+l) = 0x7F800001;
				} else abort();
				if (*t == 0) {
					if (t == end) break;
					++m, j = 0;
				} else if (*t == ':') ++j;
			}
			break;
		}
	}
	// write individual genotype information
	str = &v->indiv;
	if (v->n_sample > 0) {
		for (i = 0; i < v->n_fmt; ++i) {
			fmt_aux_t *z = &fmt[i];
			vcf_enc_int1(str, z->key);
			if ((z->y>>4&0xf) == VCF_HT_STR && !z->is_gt) {
				vcf_enc_size(str, z->size, VCF_BT_CHAR);
				kputsn((char*)z->buf, z->size * v->n_sample, str);
			} else if ((z->y>>4&0xf) == VCF_HT_INT || z->is_gt) {
				vcf_enc_vint(str, (z->size>>2) * v->n_sample, (int32_t*)z->buf, z->size>>2);
			} else {
				vcf_enc_size(str, z->size>>2, VCF_BT_FLOAT);
				kputsn((char*)z->buf, z->size * v->n_sample, str);
			}
		}
	}
	return 0;
}

int vcf_read1(vcfFile *fp, const vcf_hdr_t *h, vcf1_t *v)
{
	if (fp->is_bin) {
		uint32_t x[8];
		int ret;
		if ((ret = bgzf_read((BGZF*)fp->fp, x, 32)) != 32) {
			if (ret == 0) return -1;
			return -2;
		}
		ks_resize(&v->shared, x[0]);
		ks_resize(&v->indiv, x[1]);
		memcpy(v, x + 2, 24);
		v->shared.l = x[0], v->indiv.l = x[1];
		bgzf_read((BGZF*)fp->fp, v->shared.s, v->shared.l);
		bgzf_read((BGZF*)fp->fp, v->indiv.s, v->indiv.l);
	} else {
		int ret, dret;
		ret = ks_getuntil((kstream_t*)fp->fp, KS_SEP_LINE, &fp->line, &dret);
		if (ret < 0) return -1;
		ret = vcf_parse1(&fp->line, h, v);
	}
	return 0;
}

/**************************
 * Print VCF record lines *
 **************************/

uint8_t *vcf_unpack_fmt_core(uint8_t *ptr, int n_sample, int n_fmt, vcf_fmt_t *fmt)
{
	int i;
	for (i = 0; i < n_fmt; ++i) {
		vcf_fmt_t *f = &fmt[i];
		f->id = vcf_dec_typed_int1(ptr, &ptr);
		f->n = vcf_dec_size(ptr, &ptr, &f->type);
		f->size = f->n << vcf_type_shift[f->type];
		f->p = ptr;
		ptr += n_sample * f->size;
	}
	return ptr;
}

int vcf_format1(const vcf_hdr_t *h, const vcf1_t *v, kstring_t *s)
{
	uint8_t *ptr = (uint8_t*)v->shared.s;
	int i;
	s->l = 0;
	kputs(h->id[VCF_DT_CTG][v->rid].key, s); kputc('\t', s); // CHROM
	kputw(v->pos + 1, s); kputc('\t', s); // POS
	// ID
	ptr = vcf_fmt_sized_array(s, ptr);
	kputc('\t', s);
	if (v->n_allele) { // REF and ALT
		for (i = 0; i < v->n_allele; ++i) {
			if (i) kputc(i == 1? '\t' : ',', s);
			ptr = vcf_fmt_sized_array(s, ptr);
		}
		if (v->n_allele == 1) kputsn("\t.\t", 3, s);
		else kputc('\t', s);
	} else kputsn(".\t.\t", 4, s);
	if (memcmp(&v->qual, &vcf_missing_float, 4) == 0) kputsn(".\t", 2, s); // QUAL
	else ksprintf(s, "%g\t", v->qual);
	if (*ptr>>4) { // FILTER
		int32_t x, y;
		int type, i;
		x = vcf_dec_size(ptr, &ptr, &type);
		for (i = 0; i < x; ++i) {
			if (i) kputc(';', s);
			y = vcf_dec_int1(ptr, type, &ptr);
			kputs(h->id[VCF_DT_ID][y].key, s);
		}
		kputc('\t', s);
	} else {
		kputsn(".\t", 2, s);
		++ptr;
	}
	if (v->n_info) {
		for (i = 0; i < (int)v->n_info; ++i) {
			int32_t x;
			if (i) kputc(';', s);
			x = vcf_dec_typed_int1(ptr, &ptr);
			kputs(h->id[VCF_DT_ID][x].key, s);
			if (*ptr>>4) { // more than zero element
				kputc('=', s);
				ptr = vcf_fmt_sized_array(s, ptr);
			} else ++ptr; // skip 0
		}
	} else kputc('.', s);
	// FORMAT and individual information
	ptr = (uint8_t*)v->indiv.s;
	if (v->n_sample && v->n_fmt) { // FORMAT
		int i, j, l, gt_i = -1;
		vcf_fmt_t *fmt;
		fmt = (vcf_fmt_t*)alloca(v->n_fmt * sizeof(vcf_fmt_t));
		ptr = vcf_unpack_fmt_core(ptr, v->n_sample, v->n_fmt, fmt);
		for (i = 0; i < (int)v->n_fmt; ++i) {
			kputc(i? ':' : '\t', s);
			kputs(h->id[VCF_DT_ID][fmt[i].id].key, s);
			if (strcmp(h->id[VCF_DT_ID][fmt[i].id].key, "GT") == 0) gt_i = i;
		}
		for (j = 0; j < v->n_sample; ++j) {
			kputc('\t', s);
			for (i = 0; i < (int)v->n_fmt; ++i) {
				vcf_fmt_t *f = &fmt[i];
				if (i) kputc(':', s);
				if (gt_i == i) {
					int8_t *x = (int8_t*)(f->p + j * f->size); // FIXME: does not work with n_alt >= 64
					for (l = 0; l < f->n && x[l] != INT8_MIN; ++l) {
						if (l) kputc("/|"[x[l]&1], s);
						if (x[l]>>1) kputw((x[l]>>1) - 1, s);
						else kputc('.', s);
					}
					if (l == 0) kputc('.', s);
				} else vcf_fmt_array(s, f->n, f->type, f->p + j * f->size);
			}
		}
	}
	return 0;
}

int vcf_write1(vcfFile *fp, const vcf_hdr_t *h, const vcf1_t *v)
{
	if (fp->is_bin) {
		uint32_t x[8];
		x[0] = v->shared.l;
		x[1] = v->indiv.l;
		memcpy(x + 2, v, 24);
		bgzf_write((BGZF*)fp->fp, x, 32);
		bgzf_write((BGZF*)fp->fp, v->shared.s, v->shared.l);
		bgzf_write((BGZF*)fp->fp, v->indiv.s, v->indiv.l);
	} else {
		vcf_format1(h, v, &fp->line);
		fwrite(fp->line.s, 1, fp->line.l, (FILE*)fp->fp);
		fputc('\n', (FILE*)fp->fp);
	}
	return 0;
}

/************************
 * Data access routines *
 ************************/

int vcf_id2int(const vcf_hdr_t *h, int which, const char *id)
{
	khint_t k;
	vdict_t *d = (vdict_t*)h->dict[which];
	k = kh_get(vdict, d, id);
	return k == kh_end(d)? -1 : kh_val(d, k).id;
}

vcf_fmt_t *vcf_unpack_fmt(const vcf_hdr_t *h, const vcf1_t *v)
{
	vcf_fmt_t *fmt;
	if (v->n_fmt == 0) return 0;
	fmt = (vcf_fmt_t*)malloc(v->n_fmt * sizeof(vcf_fmt_t));
	vcf_unpack_fmt_core((uint8_t*)v->indiv.s, v->n_sample, v->n_fmt, fmt);
	return fmt;
}
