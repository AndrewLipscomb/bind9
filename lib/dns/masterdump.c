/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

/*! \file */

#include <config.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#include <isc/buffer.h>
#include <isc/event.h>
#include <isc/file.h>
#include <isc/magic.h>
#include <isc/mem.h>
#include <isc/print.h>
#include <isc/stdio.h>
#include <isc/string.h>
#include <isc/task.h>
#include <isc/time.h>
#include <isc/types.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/dbiterator.h>
#include <dns/events.h>
#include <dns/fixedname.h>
#include <dns/lib.h>
#include <dns/log.h>
#include <dns/master.h>
#include <dns/masterdump.h>
#include <dns/ncache.h>
#include <dns/rdata.h>
#include <dns/rdataclass.h>
#include <dns/rdataset.h>
#include <dns/rdatasetiter.h>
#include <dns/rdatatype.h>
#include <dns/result.h>
#include <dns/time.h>
#include <dns/ttl.h>

#define DNS_DCTX_MAGIC		ISC_MAGIC('D', 'c', 't', 'x')
#define DNS_DCTX_VALID(d)	ISC_MAGIC_VALID(d, DNS_DCTX_MAGIC)

#define RETERR(x) do { \
	isc_result_t _r = (x); \
	if (_r != ISC_R_SUCCESS) \
		return (_r); \
	} while (0)

#define CHECK(x) do { \
	if ((x) != ISC_R_SUCCESS) \
		goto cleanup; \
	} while (0)

struct dns_master_style {
	dns_masterstyle_flags_t flags;		/* DNS_STYLEFLAG_* */
	unsigned int ttl_column;
	unsigned int class_column;
	unsigned int type_column;
	unsigned int rdata_column;
	unsigned int line_length;
	unsigned int tab_width;
	unsigned int split_width;
};

/*%
 * The maximum length of the newline+indentation that is output
 * when inserting a line break in an RR.  This effectively puts an
 * upper limits on the value of "rdata_column", because if it is
 * very large, the tabs and spaces needed to reach it will not fit.
 */
#define DNS_TOTEXT_LINEBREAK_MAXLEN 100

/*%
 * Context structure for a masterfile dump in progress.
 */
typedef struct dns_totext_ctx {
	dns_master_style_t	style;
	bool 		class_printed;
	char *			linebreak;
	char 			linebreak_buf[DNS_TOTEXT_LINEBREAK_MAXLEN];
	dns_name_t *		origin;
	dns_name_t *		neworigin;
	dns_fixedname_t		origin_fixname;
	uint32_t 		current_ttl;
	bool 		current_ttl_valid;
	dns_ttl_t		serve_stale_ttl;
} dns_totext_ctx_t;

LIBDNS_EXTERNAL_DATA const dns_master_style_t
dns_master_style_keyzone = {
	DNS_STYLEFLAG_OMIT_OWNER |
	DNS_STYLEFLAG_OMIT_CLASS |
	DNS_STYLEFLAG_REL_OWNER |
	DNS_STYLEFLAG_REL_DATA |
	DNS_STYLEFLAG_OMIT_TTL |
	DNS_STYLEFLAG_TTL |
	DNS_STYLEFLAG_COMMENT |
	DNS_STYLEFLAG_RRCOMMENT |
	DNS_STYLEFLAG_MULTILINE |
	DNS_STYLEFLAG_KEYDATA,
	24, 24, 24, 32, 80, 8, UINT_MAX
};

LIBDNS_EXTERNAL_DATA const dns_master_style_t
dns_master_style_default = {
	DNS_STYLEFLAG_OMIT_OWNER |
	DNS_STYLEFLAG_OMIT_CLASS |
	DNS_STYLEFLAG_REL_OWNER |
	DNS_STYLEFLAG_REL_DATA |
	DNS_STYLEFLAG_OMIT_TTL |
	DNS_STYLEFLAG_TTL |
	DNS_STYLEFLAG_COMMENT |
	DNS_STYLEFLAG_RRCOMMENT |
	DNS_STYLEFLAG_MULTILINE,
	24, 24, 24, 32, 80, 8, UINT_MAX
};

LIBDNS_EXTERNAL_DATA const dns_master_style_t
dns_master_style_full = {
	DNS_STYLEFLAG_COMMENT |
	DNS_STYLEFLAG_RESIGN,
	46, 46, 46, 64, 120, 8, UINT_MAX
};

LIBDNS_EXTERNAL_DATA const dns_master_style_t
dns_master_style_explicitttl = {
	DNS_STYLEFLAG_OMIT_OWNER |
	DNS_STYLEFLAG_OMIT_CLASS |
	DNS_STYLEFLAG_REL_OWNER |
	DNS_STYLEFLAG_REL_DATA |
	DNS_STYLEFLAG_COMMENT |
	DNS_STYLEFLAG_RRCOMMENT |
	DNS_STYLEFLAG_MULTILINE,
	24, 32, 32, 40, 80, 8, UINT_MAX
};

LIBDNS_EXTERNAL_DATA const dns_master_style_t
dns_master_style_cache = {
	DNS_STYLEFLAG_OMIT_OWNER |
	DNS_STYLEFLAG_OMIT_CLASS |
	DNS_STYLEFLAG_MULTILINE |
	DNS_STYLEFLAG_RRCOMMENT |
	DNS_STYLEFLAG_TRUST |
	DNS_STYLEFLAG_NCACHE,
	24, 32, 32, 40, 80, 8, UINT_MAX
};

LIBDNS_EXTERNAL_DATA const dns_master_style_t
dns_master_style_simple = {
	0,
	24, 32, 32, 40, 80, 8, UINT_MAX
};

/*%
 * A style suitable for dns_rdataset_totext().
 */
LIBDNS_EXTERNAL_DATA const dns_master_style_t
dns_master_style_debug = {
	DNS_STYLEFLAG_REL_OWNER,
	24, 32, 40, 48, 80, 8, UINT_MAX
};

/*%
 * Similar, but indented (i.e., prepended with dns_master_indentstr).
 */
LIBDNS_EXTERNAL_DATA const dns_master_style_t
dns_master_style_indent = {
	DNS_STYLEFLAG_REL_OWNER |
	DNS_STYLEFLAG_INDENT,
	24, 32, 40, 48, 80, 8, UINT_MAX
};

/*%
 * Similar, but with each line commented out.
 */
LIBDNS_EXTERNAL_DATA const dns_master_style_t
dns_master_style_comment = {
	DNS_STYLEFLAG_REL_OWNER |
	DNS_STYLEFLAG_MULTILINE |
	DNS_STYLEFLAG_RRCOMMENT |
	DNS_STYLEFLAG_COMMENTDATA,
	24, 32, 40, 48, 80, 8, UINT_MAX
};

/*%
 * YAML style
 */
LIBDNS_EXTERNAL_DATA const dns_master_style_t
dns_master_style_yaml = {
	DNS_STYLEFLAG_YAML |
	DNS_STYLEFLAG_REL_OWNER |
	DNS_STYLEFLAG_INDENT,
	24, 32, 40, 48, 80, 8, UINT_MAX
};

/*%
 * Default indent string.
 */
LIBDNS_EXTERNAL_DATA const char *dns_master_indentstr = "\t";
LIBDNS_EXTERNAL_DATA unsigned int dns_master_indent = 1;

#define N_SPACES 10
static char spaces[N_SPACES+1] = "          ";

#define N_TABS 10
static char tabs[N_TABS+1] = "\t\t\t\t\t\t\t\t\t\t";

struct dns_dumpctx {
	unsigned int		magic;
	isc_mem_t		*mctx;
	isc_mutex_t		lock;
	unsigned int		references;
	bool		canceled;
	bool		first;
	bool		do_date;
	isc_stdtime_t		now;
	FILE			*f;
	dns_db_t		*db;
	dns_dbversion_t		*version;
	dns_dbiterator_t	*dbiter;
	dns_totext_ctx_t	tctx;
	isc_task_t		*task;
	dns_dumpdonefunc_t	done;
	void			*done_arg;
	unsigned int		nodes;
	/* dns_master_dumpinc() */
	char			*file;
	char 			*tmpfile;
	dns_masterformat_t	format;
	dns_masterrawheader_t	header;
	isc_result_t		(*dumpsets)(isc_mem_t *mctx,
					    const dns_name_t *name,
					    dns_rdatasetiter_t *rdsiter,
					    dns_totext_ctx_t *ctx,
					    isc_buffer_t *buffer, FILE *f);
};

#define NXDOMAIN(x) (((x)->attributes & DNS_RDATASETATTR_NXDOMAIN) != 0)

/*%
 * Output tabs and spaces to go from column '*current' to
 * column 'to', and update '*current' to reflect the new
 * current column.
 */
static isc_result_t
indent(unsigned int *current, unsigned int to, int tabwidth,
       isc_buffer_t *target)
{
	isc_region_t r;
	unsigned char *p;
	unsigned int from;
	int ntabs, nspaces, t;

	from = *current;

	if (to < from + 1)
		to = from + 1;

	ntabs = to / tabwidth - from / tabwidth;
	if (ntabs < 0)
		ntabs = 0;

	if (ntabs > 0) {
		isc_buffer_availableregion(target, &r);
		if (r.length < (unsigned) ntabs)
			return (ISC_R_NOSPACE);
		p = r.base;

		t = ntabs;
		while (t) {
			int n = t;
			if (n > N_TABS)
				n = N_TABS;
			memmove(p, tabs, n);
			p += n;
			t -= n;
		}
		isc_buffer_add(target, ntabs);
		from = (to / tabwidth) * tabwidth;
	}

	nspaces = to - from;
	INSIST(nspaces >= 0);

	isc_buffer_availableregion(target, &r);
	if (r.length < (unsigned) nspaces)
		return (ISC_R_NOSPACE);
	p = r.base;

	t = nspaces;
	while (t) {
		int n = t;
		if (n > N_SPACES)
			n = N_SPACES;
		memmove(p, spaces, n);
		p += n;
		t -= n;
	}
	isc_buffer_add(target, nspaces);

	*current = to;
	return (ISC_R_SUCCESS);
}

static isc_result_t
totext_ctx_init(const dns_master_style_t *style, dns_totext_ctx_t *ctx) {
	isc_result_t result;

	REQUIRE(style->tab_width != 0);

	ctx->style = *style;
	ctx->class_printed = false;

	dns_fixedname_init(&ctx->origin_fixname);

	/*
	 * Set up the line break string if needed.
	 */
	if ((ctx->style.flags & DNS_STYLEFLAG_MULTILINE) != 0) {
		isc_buffer_t buf;
		isc_region_t r;
		unsigned int col = 0;

		isc_buffer_init(&buf, ctx->linebreak_buf,
				sizeof(ctx->linebreak_buf));

		isc_buffer_availableregion(&buf, &r);
		if (r.length < 1)
			return (DNS_R_TEXTTOOLONG);
		r.base[0] = '\n';
		isc_buffer_add(&buf, 1);

		if ((ctx->style.flags & DNS_STYLEFLAG_INDENT) != 0 ||
		    (ctx->style.flags & DNS_STYLEFLAG_YAML) != 0)
		{
			unsigned int i, len = strlen(dns_master_indentstr);
			for (i = 0; i < dns_master_indent; i++) {
				if (isc_buffer_availablelength(&buf) < len)
					return (DNS_R_TEXTTOOLONG);
				isc_buffer_putstr(&buf, dns_master_indentstr);
			}
		}

		if ((ctx->style.flags & DNS_STYLEFLAG_COMMENTDATA) != 0) {
			isc_buffer_availableregion(&buf, &r);
			if (r.length < 1)
				return (DNS_R_TEXTTOOLONG);
			r.base[0] = ';';
			isc_buffer_add(&buf, 1);
		}

		result = indent(&col, ctx->style.rdata_column,
				ctx->style.tab_width, &buf);
		/*
		 * Do not return ISC_R_NOSPACE if the line break string
		 * buffer is too small, because that would just make
		 * dump_rdataset() retry indefinitely with ever
		 * bigger target buffers.  That's a different buffer,
		 * so it won't help.  Use DNS_R_TEXTTOOLONG as a substitute.
		 */
		if (result == ISC_R_NOSPACE)
			return (DNS_R_TEXTTOOLONG);
		if (result != ISC_R_SUCCESS)
			return (result);

		isc_buffer_availableregion(&buf, &r);
		if (r.length < 1)
			return (DNS_R_TEXTTOOLONG);
		r.base[0] = '\0';
		isc_buffer_add(&buf, 1);
		ctx->linebreak = ctx->linebreak_buf;
	} else {
		ctx->linebreak = NULL;
	}

	ctx->origin = NULL;
	ctx->neworigin = NULL;
	ctx->current_ttl = 0;
	ctx->current_ttl_valid = false;
	ctx->serve_stale_ttl = 0;

	return (ISC_R_SUCCESS);
}

#define INDENT_TO(col) \
	do { \
		 if ((ctx->style.flags & DNS_STYLEFLAG_YAML) != 0) { \
			if ((result = str_totext(" ", target)) \
			    != ISC_R_SUCCESS) \
				return (result); \
		 } else if ((result = indent(&column, ctx->style.col, \
				      ctx->style.tab_width, target)) \
		     != ISC_R_SUCCESS) \
			    return (result); \
	} while (0)


static isc_result_t
str_totext(const char *source, isc_buffer_t *target) {
	unsigned int l;
	isc_region_t region;

	isc_buffer_availableregion(target, &region);
	l = strlen(source);

	if (l > region.length)
		return (ISC_R_NOSPACE);

	memmove(region.base, source, l);
	isc_buffer_add(target, l);
	return (ISC_R_SUCCESS);
}

static isc_result_t
ncache_summary(dns_rdataset_t *rdataset, bool omit_final_dot,
	       isc_buffer_t *target)
{
	isc_result_t result = ISC_R_SUCCESS;
	dns_rdataset_t rds;
	dns_name_t name;

	dns_rdataset_init(&rds);
	dns_name_init(&name, NULL);

	do {
		dns_ncache_current(rdataset, &name, &rds);
		for (result = dns_rdataset_first(&rds);
		     result == ISC_R_SUCCESS;
		     result = dns_rdataset_next(&rds)) {
			CHECK(str_totext("; ", target));
			CHECK(dns_name_totext(&name, omit_final_dot, target));
			CHECK(str_totext(" ", target));
			CHECK(dns_rdatatype_totext(rds.type, target));
			if (rds.type == dns_rdatatype_rrsig) {
				CHECK(str_totext(" ", target));
				CHECK(dns_rdatatype_totext(rds.covers, target));
				CHECK(str_totext(" ...\n", target));
			} else {
				dns_rdata_t rdata = DNS_RDATA_INIT;
				dns_rdataset_current(&rds, &rdata);
				CHECK(str_totext(" ", target));
				CHECK(dns_rdata_tofmttext(&rdata, dns_rootname,
							  0, 0, 0, " ", target));
				CHECK(str_totext("\n", target));
			}
		}
		dns_rdataset_disassociate(&rds);
		result = dns_rdataset_next(rdataset);
	} while (result == ISC_R_SUCCESS);

	if (result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;
 cleanup:
	if (dns_rdataset_isassociated(&rds))
		dns_rdataset_disassociate(&rds);

	return (result);
}

/*
 * Convert 'rdataset' to master file text format according to 'ctx',
 * storing the result in 'target'.  If 'owner_name' is NULL, it
 * is omitted; otherwise 'owner_name' must be valid and have at least
 * one label.
 */

static isc_result_t
rdataset_totext(dns_rdataset_t *rdataset,
		const dns_name_t *owner_name,
		dns_totext_ctx_t *ctx,
		bool omit_final_dot,
		isc_buffer_t *target)
{
	isc_result_t result;
	unsigned int column;
	bool first = true;
	uint32_t current_ttl;
	bool current_ttl_valid;
	dns_rdatatype_t type;
	unsigned int type_start;
	dns_fixedname_t fixed;
	dns_name_t *name = NULL;
	unsigned int i;

	REQUIRE(DNS_RDATASET_VALID(rdataset));

	rdataset->attributes |= DNS_RDATASETATTR_LOADORDER;
	result = dns_rdataset_first(rdataset);

	current_ttl = ctx->current_ttl;
	current_ttl_valid = ctx->current_ttl_valid;

	if (owner_name != NULL) {
		name = dns_fixedname_initname(&fixed);
		dns_name_copy(owner_name, name, NULL);
		dns_rdataset_getownercase(rdataset, name);
	}

	while (result == ISC_R_SUCCESS) {
		column = 0;

		/*
		 * Indent?
		 */
		if ((ctx->style.flags & DNS_STYLEFLAG_INDENT) != 0 ||
		    (ctx->style.flags & DNS_STYLEFLAG_YAML) != 0)
			for (i = 0; i < dns_master_indent; i++)
				RETERR(str_totext(dns_master_indentstr,
						  target));

		/*
		 * YAML enumerator?
		 */
		if ((ctx->style.flags & DNS_STYLEFLAG_YAML) != 0) {
			RETERR(str_totext("- ", target));
		}

		/*
		 * Comment?
		 */
		if ((ctx->style.flags & DNS_STYLEFLAG_COMMENTDATA) != 0)
			RETERR(str_totext(";", target));

		/*
		 * Owner name.
		 */
		if (name != NULL &&
		    ! ((ctx->style.flags & DNS_STYLEFLAG_OMIT_OWNER) != 0 &&
		       !first))
		{
			unsigned int name_start = target->used;
			RETERR(dns_name_totext(name, omit_final_dot, target));
			column += target->used - name_start;
		}

		/*
		 * TTL.
		 */
		if ((ctx->style.flags & DNS_STYLEFLAG_NO_TTL) == 0 &&
		    !((ctx->style.flags & DNS_STYLEFLAG_OMIT_TTL) != 0 &&
		      current_ttl_valid &&
		      rdataset->ttl == current_ttl))
		{
			char ttlbuf[64];
			isc_region_t r;
			unsigned int length;

			INDENT_TO(ttl_column);
			if ((ctx->style.flags & DNS_STYLEFLAG_TTL_UNITS) != 0) {
				length = target->used;
				result = dns_ttl_totext(rdataset->ttl,
							false, false,
							target);
				if (result != ISC_R_SUCCESS)
					return (result);
				column += target->used - length;
			} else {
				length = snprintf(ttlbuf, sizeof(ttlbuf), "%u",
						  rdataset->ttl);
				INSIST(length <= sizeof(ttlbuf));
				isc_buffer_availableregion(target, &r);
				if (r.length < length)
					return (ISC_R_NOSPACE);
				memmove(r.base, ttlbuf, length);
				isc_buffer_add(target, length);
				column += length;
			}

			/*
			 * If the $TTL directive is not in use, the TTL we
			 * just printed becomes the default for subsequent RRs.
			 */
			if ((ctx->style.flags & DNS_STYLEFLAG_TTL) == 0) {
				current_ttl = rdataset->ttl;
				current_ttl_valid = true;
			}
		}

		/*
		 * Class.
		 */
		if ((ctx->style.flags & DNS_STYLEFLAG_NO_CLASS) == 0 &&
		    ((ctx->style.flags & DNS_STYLEFLAG_OMIT_CLASS) == 0 ||
		     ctx->class_printed == false))
		{
			unsigned int class_start;
			INDENT_TO(class_column);
			class_start = target->used;
			if ((ctx->style.flags & DNS_STYLEFLAG_UNKNOWNFORMAT) != 0)
				result = dns_rdataclass_tounknowntext
					(rdataset->rdclass, target);
			else
				result = dns_rdataclass_totext
					(rdataset->rdclass, target);
			if (result != ISC_R_SUCCESS)
				return (result);
			column += (target->used - class_start);
		}

		/*
		 * Type.
		 */

		if ((rdataset->attributes & DNS_RDATASETATTR_NEGATIVE) != 0) {
			type = rdataset->covers;
		} else {
			type = rdataset->type;
		}

		INDENT_TO(type_column);
		type_start = target->used;
		if ((rdataset->attributes & DNS_RDATASETATTR_NEGATIVE) != 0)
			RETERR(str_totext("\\-", target));
		switch (type) {
		case dns_rdatatype_keydata:
#define KEYDATA "KEYDATA"
			if ((ctx->style.flags & DNS_STYLEFLAG_KEYDATA) != 0) {
				if (isc_buffer_availablelength(target) <
				    (sizeof(KEYDATA) - 1))
					return (ISC_R_NOSPACE);
				isc_buffer_putstr(target, KEYDATA);
				break;
			}
			/* FALLTHROUGH */
		default:
			if ((ctx->style.flags & DNS_STYLEFLAG_UNKNOWNFORMAT) != 0)
				result = dns_rdatatype_tounknowntext(type, target);
			else
				result = dns_rdatatype_totext(type, target);
			if (result != ISC_R_SUCCESS)
				return (result);
		}
		column += (target->used - type_start);

		/*
		 * Rdata.
		 */
		INDENT_TO(rdata_column);
		if ((rdataset->attributes & DNS_RDATASETATTR_NEGATIVE) != 0) {
			if ((ctx->style.flags & DNS_STYLEFLAG_INDENT) != 0 ||
			    (ctx->style.flags & DNS_STYLEFLAG_YAML) != 0)
			{
				for (i = 0; i < dns_master_indent; i++)
					RETERR(str_totext(dns_master_indentstr,
							  target));
			}
			if (NXDOMAIN(rdataset))
				RETERR(str_totext(";-$NXDOMAIN\n", target));
			else
				RETERR(str_totext(";-$NXRRSET\n", target));
			/*
			 * Print a summary of the cached records which make
			 * up the negative response.
			 */
			RETERR(ncache_summary(rdataset, omit_final_dot,
					      target));
			break;
		} else {
			dns_rdata_t rdata = DNS_RDATA_INIT;
			isc_region_t r;

			dns_rdataset_current(rdataset, &rdata);

			RETERR(dns_rdata_tofmttext(&rdata,
						   ctx->origin,
						   ctx->style.flags,
						   ctx->style.line_length -
						       ctx->style.rdata_column,
						   ctx->style.split_width,
						   ctx->linebreak,
						   target));

			isc_buffer_availableregion(target, &r);
			if (r.length < 1)
				return (ISC_R_NOSPACE);
			r.base[0] = '\n';
			isc_buffer_add(target, 1);
		}

		first = false;
		result = dns_rdataset_next(rdataset);
	}

	if (result != ISC_R_NOMORE)
		return (result);

	/*
	 * Update the ctx state to reflect what we just printed.
	 * This is done last, only when we are sure we will return
	 * success, because this function may be called multiple
	 * times with increasing buffer sizes until it succeeds,
	 * and failed attempts must not update the state prematurely.
	 */
	ctx->class_printed = true;
	ctx->current_ttl= current_ttl;
	ctx->current_ttl_valid = current_ttl_valid;

	return (ISC_R_SUCCESS);
}

/*
 * Print the name, type, and class of an empty rdataset,
 * such as those used to represent the question section
 * of a DNS message.
 */
static isc_result_t
question_totext(dns_rdataset_t *rdataset,
		const dns_name_t *owner_name,
		dns_totext_ctx_t *ctx,
		bool omit_final_dot,
		isc_buffer_t *target)
{
	unsigned int column;
	isc_result_t result;
	isc_region_t r;

	REQUIRE(DNS_RDATASET_VALID(rdataset));
	result = dns_rdataset_first(rdataset);
	REQUIRE(result == ISC_R_NOMORE);

	column = 0;

	/* Owner name */
	{
		unsigned int name_start = target->used;
		RETERR(dns_name_totext(owner_name,
				       omit_final_dot,
				       target));
		column += target->used - name_start;
	}

	/* Class */
	{
		unsigned int class_start;
		INDENT_TO(class_column);
		class_start = target->used;
		if ((ctx->style.flags & DNS_STYLEFLAG_UNKNOWNFORMAT) != 0)
			result = dns_rdataclass_tounknowntext(rdataset->rdclass,
							      target);
		else
			result = dns_rdataclass_totext(rdataset->rdclass,
						       target);
		if (result != ISC_R_SUCCESS)
			return (result);
		column += (target->used - class_start);
	}

	/* Type */
	{
		unsigned int type_start;
		INDENT_TO(type_column);
		type_start = target->used;
		if ((ctx->style.flags & DNS_STYLEFLAG_UNKNOWNFORMAT) != 0)
			result = dns_rdatatype_tounknowntext(rdataset->type,
							     target);
		else
			result = dns_rdatatype_totext(rdataset->type,
						      target);
		if (result != ISC_R_SUCCESS)
			return (result);
		column += (target->used - type_start);
	}

	isc_buffer_availableregion(target, &r);
	if (r.length < 1)
		return (ISC_R_NOSPACE);
	r.base[0] = '\n';
	isc_buffer_add(target, 1);

	return (ISC_R_SUCCESS);
}

isc_result_t
dns_rdataset_totext(dns_rdataset_t *rdataset,
		    const dns_name_t *owner_name,
		    bool omit_final_dot,
		    bool question,
		    isc_buffer_t *target)
{
	dns_totext_ctx_t ctx;
	isc_result_t result;
	result = totext_ctx_init(&dns_master_style_debug, &ctx);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "could not set master file style");
		return (ISC_R_UNEXPECTED);
	}

	/*
	 * The caller might want to give us an empty owner
	 * name (e.g. if they are outputting into a master
	 * file and this rdataset has the same name as the
	 * previous one.)
	 */
	if (dns_name_countlabels(owner_name) == 0)
		owner_name = NULL;

	if (question)
		return (question_totext(rdataset, owner_name, &ctx,
					omit_final_dot, target));
	else
		return (rdataset_totext(rdataset, owner_name, &ctx,
					omit_final_dot, target));
}

isc_result_t
dns_master_rdatasettotext(const dns_name_t *owner_name,
			  dns_rdataset_t *rdataset,
			  const dns_master_style_t *style,
			  isc_buffer_t *target)
{
	dns_totext_ctx_t ctx;
	isc_result_t result;
	result = totext_ctx_init(style, &ctx);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "could not set master file style");
		return (ISC_R_UNEXPECTED);
	}

	return (rdataset_totext(rdataset, owner_name, &ctx,
				false, target));
}

isc_result_t
dns_master_questiontotext(const dns_name_t *owner_name,
			  dns_rdataset_t *rdataset,
			  const dns_master_style_t *style,
			  isc_buffer_t *target)
{
	dns_totext_ctx_t ctx;
	isc_result_t result;
	result = totext_ctx_init(style, &ctx);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "could not set master file style");
		return (ISC_R_UNEXPECTED);
	}

	return (question_totext(rdataset, owner_name, &ctx,
				false, target));
}

/*
 * Print an rdataset.  'buffer' is a scratch buffer, which must have been
 * dynamically allocated by the caller.  It must be large enough to
 * hold the result from dns_ttl_totext().  If more than that is needed,
 * the buffer will be grown automatically.
 */

static isc_result_t
dump_rdataset(isc_mem_t *mctx, const dns_name_t *name,
	      dns_rdataset_t *rdataset, dns_totext_ctx_t *ctx,
	      isc_buffer_t *buffer, FILE *f)
{
	isc_region_t r;
	isc_result_t result;

	REQUIRE(buffer->length > 0);

	/*
	 * Output a $TTL directive if needed.
	 */

	if ((ctx->style.flags & DNS_STYLEFLAG_TTL) != 0) {
		if (ctx->current_ttl_valid == false ||
		    ctx->current_ttl != rdataset->ttl)
		{
			if ((ctx->style.flags & DNS_STYLEFLAG_COMMENT) != 0)
			{
				isc_buffer_clear(buffer);
				result = dns_ttl_totext(rdataset->ttl,
							true, true,
							buffer);
				INSIST(result == ISC_R_SUCCESS);
				isc_buffer_usedregion(buffer, &r);
				fprintf(f, "$TTL %u\t; %.*s\n", rdataset->ttl,
					(int) r.length, (char *) r.base);
			} else {
				fprintf(f, "$TTL %u\n", rdataset->ttl);
			}
			ctx->current_ttl = rdataset->ttl;
			ctx->current_ttl_valid = true;
		}
	}

	isc_buffer_clear(buffer);

	/*
	 * Generate the text representation of the rdataset into
	 * the buffer.  If the buffer is too small, grow it.
	 */
	for (;;) {
		int newlength;
		void *newmem;
		result = rdataset_totext(rdataset, name, ctx,
					 false, buffer);
		if (result != ISC_R_NOSPACE)
			break;

		newlength = buffer->length * 2;
		newmem = isc_mem_get(mctx, newlength);
		if (newmem == NULL)
			return (ISC_R_NOMEMORY);
		isc_mem_put(mctx, buffer->base, buffer->length);
		isc_buffer_init(buffer, newmem, newlength);
	}
	if (result != ISC_R_SUCCESS)
		return (result);

	/*
	 * Write the buffer contents to the master file.
	 */
	isc_buffer_usedregion(buffer, &r);
	result = isc_stdio_write(r.base, 1, (size_t)r.length, f, NULL);

	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "master file write failed: %s",
				 isc_result_totext(result));
		return (result);
	}

	return (ISC_R_SUCCESS);
}

/*
 * Define the order in which rdatasets should be printed in zone
 * files.  We will print SOA and NS records before others, SIGs
 * immediately following the things they sign, and order everything
 * else by RR number.  This is all just for aesthetics and
 * compatibility with buggy software that expects the SOA to be first;
 * the DNS specifications allow any order.
 */

static int
dump_order(const dns_rdataset_t *rds) {
	int t;
	int sig;
	if (rds->type == dns_rdatatype_rrsig) {
		t = rds->covers;
		sig = 1;
	} else {
		t = rds->type;
		sig = 0;
	}
	switch (t) {
	case dns_rdatatype_soa:
		t = 0;
		break;
	case dns_rdatatype_ns:
		t = 1;
		break;
	default:
		t += 2;
		break;
	}
	return (t << 1) + sig;
}

static int
dump_order_compare(const void *a, const void *b) {
	return (dump_order(*((const dns_rdataset_t * const *) a)) -
		dump_order(*((const dns_rdataset_t * const *) b)));
}

/*
 * Dump all the rdatasets of a domain name to a master file.  We make
 * a "best effort" attempt to sort the RRsets in a nice order, but if
 * there are more than MAXSORT RRsets, we punt and only sort them in
 * groups of MAXSORT.  This is not expected to ever happen in practice
 * since much less than 64 RR types have been registered with the
 * IANA, so far, and the output will be correct (though not
 * aesthetically pleasing) even if it does happen.
 */

#define MAXSORT 64

static isc_result_t
dump_rdatasets_text(isc_mem_t *mctx, const dns_name_t *name,
		    dns_rdatasetiter_t *rdsiter, dns_totext_ctx_t *ctx,
		    isc_buffer_t *buffer, FILE *f)
{
	isc_result_t itresult, dumpresult;
	isc_region_t r;
	dns_rdataset_t rdatasets[MAXSORT];
	dns_rdataset_t *sorted[MAXSORT];
	int i, n;

	itresult = dns_rdatasetiter_first(rdsiter);
	dumpresult = ISC_R_SUCCESS;

	if (itresult == ISC_R_SUCCESS && ctx->neworigin != NULL) {
		isc_buffer_clear(buffer);
		itresult = dns_name_totext(ctx->neworigin, false, buffer);
		RUNTIME_CHECK(itresult == ISC_R_SUCCESS);
		isc_buffer_usedregion(buffer, &r);
		fprintf(f, "$ORIGIN %.*s\n", (int) r.length, (char *) r.base);
		ctx->neworigin = NULL;
	}

 again:
	for (i = 0;
	     itresult == ISC_R_SUCCESS && i < MAXSORT;
	     itresult = dns_rdatasetiter_next(rdsiter), i++) {
		dns_rdataset_init(&rdatasets[i]);
		dns_rdatasetiter_current(rdsiter, &rdatasets[i]);
		sorted[i] = &rdatasets[i];
	}
	n = i;
	INSIST(n <= MAXSORT);

	qsort(sorted, n, sizeof(sorted[0]), dump_order_compare);

	for (i = 0; i < n; i++) {
		dns_rdataset_t *rds = sorted[i];
		if ((ctx->style.flags & DNS_STYLEFLAG_TRUST) != 0) {
			if ((ctx->style.flags & DNS_STYLEFLAG_INDENT) != 0 ||
			    (ctx->style.flags & DNS_STYLEFLAG_YAML) != 0)
			{
				unsigned int j;
				for (j = 0; j < dns_master_indent; j++)
					fprintf(f, "%s", dns_master_indentstr);
			}
			fprintf(f, "; %s\n", dns_trust_totext(rds->trust));
		}
		if (((rds->attributes & DNS_RDATASETATTR_NEGATIVE) != 0) &&
		    (ctx->style.flags & DNS_STYLEFLAG_NCACHE) == 0) {
			/* Omit negative cache entries */
		} else {
			isc_result_t result;
			if (rds->ttl < ctx->serve_stale_ttl)
				fprintf(f, "; stale\n");
			result = dump_rdataset(mctx, name, rds, ctx, buffer, f);
			if (result != ISC_R_SUCCESS)
				dumpresult = result;
			if ((ctx->style.flags & DNS_STYLEFLAG_OMIT_OWNER) != 0)
				name = NULL;
		}
		if (((ctx->style.flags & DNS_STYLEFLAG_RESIGN) != 0) &&
		    ((rds->attributes & DNS_RDATASETATTR_RESIGN) != 0)) {
			isc_buffer_t b;
			char buf[sizeof("YYYYMMDDHHMMSS")];
			memset(buf, 0, sizeof(buf));
			isc_buffer_init(&b, buf, sizeof(buf) - 1);
			dns_time64_totext((uint64_t)rds->resign, &b);
			if ((ctx->style.flags & DNS_STYLEFLAG_INDENT) != 0 ||
			    (ctx->style.flags & DNS_STYLEFLAG_YAML) != 0)
			{
				unsigned int j;
				for (j = 0; j < dns_master_indent; j++)
					fprintf(f, "%s", dns_master_indentstr);
			}
			fprintf(f, "; resign=%s\n", buf);
		}
		dns_rdataset_disassociate(rds);
	}

	if (dumpresult != ISC_R_SUCCESS)
		return (dumpresult);

	/*
	 * If we got more data than could be sorted at once,
	 * go handle the rest.
	 */
	if (itresult == ISC_R_SUCCESS)
		goto again;

	if (itresult == ISC_R_NOMORE)
		itresult = ISC_R_SUCCESS;

	return (itresult);
}

/*
 * Dump given RRsets in the "raw" format.
 */
static isc_result_t
dump_rdataset_raw(isc_mem_t *mctx, const dns_name_t *name,
		  dns_rdataset_t *rdataset, isc_buffer_t *buffer, FILE *f)
{
	isc_result_t result;
	uint32_t totallen;
	uint16_t dlen;
	isc_region_t r, r_hdr;

	REQUIRE(buffer->length > 0);
	REQUIRE(DNS_RDATASET_VALID(rdataset));

	rdataset->attributes |= DNS_RDATASETATTR_LOADORDER;
 restart:
	totallen = 0;
	result = dns_rdataset_first(rdataset);
	REQUIRE(result == ISC_R_SUCCESS);

	isc_buffer_clear(buffer);

	/*
	 * Common header and owner name (length followed by name)
	 * These fields should be in a moderate length, so we assume we
	 * can store all of them in the initial buffer.
	 */
	isc_buffer_availableregion(buffer, &r_hdr);
	INSIST(r_hdr.length >= sizeof(dns_masterrawrdataset_t));
	isc_buffer_putuint32(buffer, totallen);	/* XXX: leave space */
	isc_buffer_putuint16(buffer, rdataset->rdclass); /* 16-bit class */
	isc_buffer_putuint16(buffer, rdataset->type); /* 16-bit type */
	isc_buffer_putuint16(buffer, rdataset->covers);	/* same as type */
	isc_buffer_putuint32(buffer, rdataset->ttl); /* 32-bit TTL */
	isc_buffer_putuint32(buffer, dns_rdataset_count(rdataset));
	totallen = isc_buffer_usedlength(buffer);
	INSIST(totallen <= sizeof(dns_masterrawrdataset_t));

	dns_name_toregion(name, &r);
	INSIST(isc_buffer_availablelength(buffer) >=
	       (sizeof(dlen) + r.length));
	dlen = (uint16_t)r.length;
	isc_buffer_putuint16(buffer, dlen);
	isc_buffer_copyregion(buffer, &r);
	totallen += sizeof(dlen) + r.length;

	do {
		dns_rdata_t rdata = DNS_RDATA_INIT;

		dns_rdataset_current(rdataset, &rdata);
		dns_rdata_toregion(&rdata, &r);
		INSIST(r.length <= 0xffffU);
		dlen = (uint16_t)r.length;

		/*
		 * Copy the rdata into the buffer.  If the buffer is too small,
		 * grow it.  This should be rare, so we'll simply restart the
		 * entire procedure (or should we copy the old data and
		 * continue?).
		 */
		if (isc_buffer_availablelength(buffer) <
						 sizeof(dlen) + r.length) {
			int newlength;
			void *newmem;

			newlength = buffer->length * 2;
			newmem = isc_mem_get(mctx, newlength);
			if (newmem == NULL)
				return (ISC_R_NOMEMORY);
			isc_mem_put(mctx, buffer->base, buffer->length);
			isc_buffer_init(buffer, newmem, newlength);
			goto restart;
		}
		isc_buffer_putuint16(buffer, dlen);
		isc_buffer_copyregion(buffer, &r);
		totallen += sizeof(dlen) + r.length;

		result = dns_rdataset_next(rdataset);
	} while (result == ISC_R_SUCCESS);

	if (result != ISC_R_NOMORE)
		return (result);

	/*
	 * Fill in the total length field.
	 * XXX: this is a bit tricky.  Since we have already "used" the space
	 * for the total length in the buffer, we first remember the entire
	 * buffer length in the region, "rewind", and then write the value.
	 */
	isc_buffer_usedregion(buffer, &r);
	isc_buffer_clear(buffer);
	isc_buffer_putuint32(buffer, totallen);
	INSIST(isc_buffer_usedlength(buffer) < totallen);

	/*
	 * Write the buffer contents to the raw master file.
	 */
	result = isc_stdio_write(r.base, 1, (size_t)r.length, f, NULL);

	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "raw master file write failed: %s",
				 isc_result_totext(result));
		return (result);
	}

	return (result);
}

static isc_result_t
dump_rdatasets_raw(isc_mem_t *mctx, const dns_name_t *owner_name,
		   dns_rdatasetiter_t *rdsiter, dns_totext_ctx_t *ctx,
		   isc_buffer_t *buffer, FILE *f)
{
	isc_result_t result;
	dns_rdataset_t rdataset;
	dns_fixedname_t fixed;
	dns_name_t *name;

	name = dns_fixedname_initname(&fixed);
	dns_name_copy(owner_name, name, NULL);
	for (result = dns_rdatasetiter_first(rdsiter);
	     result == ISC_R_SUCCESS;
	     result = dns_rdatasetiter_next(rdsiter)) {

		dns_rdataset_init(&rdataset);
		dns_rdatasetiter_current(rdsiter, &rdataset);

		dns_rdataset_getownercase(&rdataset, name);

		if (((rdataset.attributes & DNS_RDATASETATTR_NEGATIVE) != 0) &&
		    (ctx->style.flags & DNS_STYLEFLAG_NCACHE) == 0) {
			/* Omit negative cache entries */
		} else {
			result = dump_rdataset_raw(mctx, name, &rdataset,
						   buffer, f);
		}
		dns_rdataset_disassociate(&rdataset);
		if (result != ISC_R_SUCCESS)
			return (result);
	}

	if (result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;

	return (result);
}

static isc_result_t
dump_rdatasets_map(isc_mem_t *mctx, const dns_name_t *name,
		   dns_rdatasetiter_t *rdsiter, dns_totext_ctx_t *ctx,
		   isc_buffer_t *buffer, FILE *f)
{
	UNUSED(mctx);
	UNUSED(name);
	UNUSED(rdsiter);
	UNUSED(ctx);
	UNUSED(buffer);
	UNUSED(f);

	return (ISC_R_NOTIMPLEMENTED);
}

/*
 * Initial size of text conversion buffer.  The buffer is used
 * for several purposes: converting origin names, rdatasets,
 * $DATE timestamps, and comment strings for $TTL directives.
 *
 * When converting rdatasets, it is dynamically resized, but
 * when converting origins, timestamps, etc it is not.  Therefore,
 * the initial size must large enough to hold the longest possible
 * text representation of any domain name (for $ORIGIN).
 */
static const int initial_buffer_length = 1200;

static isc_result_t
dumptostreaminc(dns_dumpctx_t *dctx);

static void
dumpctx_destroy(dns_dumpctx_t *dctx) {

	dctx->magic = 0;
	DESTROYLOCK(&dctx->lock);
	dns_dbiterator_destroy(&dctx->dbiter);
	if (dctx->version != NULL)
		dns_db_closeversion(dctx->db, &dctx->version, false);
	dns_db_detach(&dctx->db);
	if (dctx->task != NULL)
		isc_task_detach(&dctx->task);
	if (dctx->file != NULL)
		isc_mem_free(dctx->mctx, dctx->file);
	if (dctx->tmpfile != NULL)
		isc_mem_free(dctx->mctx, dctx->tmpfile);
	isc_mem_putanddetach(&dctx->mctx, dctx, sizeof(*dctx));
}

void
dns_dumpctx_attach(dns_dumpctx_t *source, dns_dumpctx_t **target) {

	REQUIRE(DNS_DCTX_VALID(source));
	REQUIRE(target != NULL && *target == NULL);

	LOCK(&source->lock);
	INSIST(source->references > 0);
	source->references++;
	INSIST(source->references != 0);	/* Overflow? */
	UNLOCK(&source->lock);

	*target = source;
}

void
dns_dumpctx_detach(dns_dumpctx_t **dctxp) {
	dns_dumpctx_t *dctx;
	bool need_destroy = false;

	REQUIRE(dctxp != NULL);
	dctx = *dctxp;
	REQUIRE(DNS_DCTX_VALID(dctx));

	*dctxp = NULL;

	LOCK(&dctx->lock);
	INSIST(dctx->references != 0);
	dctx->references--;
	if (dctx->references == 0)
		need_destroy = true;
	UNLOCK(&dctx->lock);
	if (need_destroy)
		dumpctx_destroy(dctx);
}

dns_dbversion_t *
dns_dumpctx_version(dns_dumpctx_t *dctx) {
	REQUIRE(DNS_DCTX_VALID(dctx));
	return (dctx->version);
}

dns_db_t *
dns_dumpctx_db(dns_dumpctx_t *dctx) {
	REQUIRE(DNS_DCTX_VALID(dctx));
	return (dctx->db);
}

void
dns_dumpctx_cancel(dns_dumpctx_t *dctx) {
	REQUIRE(DNS_DCTX_VALID(dctx));

	LOCK(&dctx->lock);
	dctx->canceled = true;
	UNLOCK(&dctx->lock);
}

static isc_result_t
flushandsync(FILE *f, isc_result_t result, const char *temp) {
	bool logit = (result == ISC_R_SUCCESS);

	if (result == ISC_R_SUCCESS)
		result = isc_stdio_flush(f);
	if (result != ISC_R_SUCCESS && logit) {
		if (temp != NULL)
			isc_log_write(dns_lctx, ISC_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_MASTERDUMP, ISC_LOG_ERROR,
				      "dumping to master file: %s: flush: %s",
				      temp, isc_result_totext(result));
		else
			isc_log_write(dns_lctx, ISC_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_MASTERDUMP, ISC_LOG_ERROR,
				      "dumping to stream: flush: %s",
				      isc_result_totext(result));
		logit = false;
	}

	if (result == ISC_R_SUCCESS)
		result = isc_stdio_sync(f);
	if (result != ISC_R_SUCCESS && logit) {
		if (temp != NULL)
			isc_log_write(dns_lctx, ISC_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_MASTERDUMP, ISC_LOG_ERROR,
				      "dumping to master file: %s: fsync: %s",
				      temp, isc_result_totext(result));
		else
			isc_log_write(dns_lctx, ISC_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_MASTERDUMP, ISC_LOG_ERROR,
				      "dumping to stream: fsync: %s",
				      isc_result_totext(result));
	}
	return (result);
}

static isc_result_t
closeandrename(FILE *f, isc_result_t result, const char *temp, const char *file)
{
	isc_result_t tresult;
	bool logit = (result == ISC_R_SUCCESS);

	result = flushandsync(f, result, temp);
	if (result != ISC_R_SUCCESS)
		logit = false;

	tresult = isc_stdio_close(f);
	if (result == ISC_R_SUCCESS)
		result = tresult;
	if (result != ISC_R_SUCCESS && logit) {
		isc_log_write(dns_lctx, ISC_LOGCATEGORY_GENERAL,
			      DNS_LOGMODULE_MASTERDUMP, ISC_LOG_ERROR,
			      "dumping master file: %s: fclose: %s",
			      temp, isc_result_totext(result));
		logit = false;
	}
	if (result == ISC_R_SUCCESS)
		result = isc_file_rename(temp, file);
	else
		(void)isc_file_remove(temp);
	if (result != ISC_R_SUCCESS && logit) {
		isc_log_write(dns_lctx, ISC_LOGCATEGORY_GENERAL,
			      DNS_LOGMODULE_MASTERDUMP, ISC_LOG_ERROR,
			      "dumping master file: rename: %s: %s",
			      file, isc_result_totext(result));
	}
	return (result);
}

static void
dump_quantum(isc_task_t *task, isc_event_t *event) {
	isc_result_t result;
	isc_result_t tresult;
	dns_dumpctx_t *dctx;

	REQUIRE(event != NULL);
	dctx = event->ev_arg;
	REQUIRE(DNS_DCTX_VALID(dctx));
	if (dctx->canceled)
		result = ISC_R_CANCELED;
	else
		result = dumptostreaminc(dctx);
	if (result == DNS_R_CONTINUE) {
		event->ev_arg = dctx;
		isc_task_send(task, &event);
		return;
	}

	if (dctx->file != NULL) {
		tresult = closeandrename(dctx->f, result,
					 dctx->tmpfile, dctx->file);
		if (tresult != ISC_R_SUCCESS && result == ISC_R_SUCCESS)
			result = tresult;
	} else
		result = flushandsync(dctx->f, result, NULL);
	(dctx->done)(dctx->done_arg, result);
	isc_event_free(&event);
	dns_dumpctx_detach(&dctx);
}

static isc_result_t
task_send(dns_dumpctx_t *dctx) {
	isc_event_t *event;

	event = isc_event_allocate(dctx->mctx, NULL, DNS_EVENT_DUMPQUANTUM,
				   dump_quantum, dctx, sizeof(*event));
	if (event == NULL)
		return (ISC_R_NOMEMORY);
	isc_task_send(dctx->task, &event);
	return (ISC_R_SUCCESS);
}

static isc_result_t
dumpctx_create(isc_mem_t *mctx, dns_db_t *db, dns_dbversion_t *version,
	       const dns_master_style_t *style, FILE *f, dns_dumpctx_t **dctxp,
	       dns_masterformat_t format, dns_masterrawheader_t *header)
{
	dns_dumpctx_t *dctx;
	isc_result_t result;
	unsigned int options;

	dctx = isc_mem_get(mctx, sizeof(*dctx));
	if (dctx == NULL)
		return (ISC_R_NOMEMORY);

	dctx->mctx = NULL;
	dctx->f = f;
	dctx->dbiter = NULL;
	dctx->db = NULL;
	dctx->version = NULL;
	dctx->done = NULL;
	dctx->done_arg = NULL;
	dctx->task = NULL;
	dctx->nodes = 0;
	dctx->first = true;
	dctx->canceled = false;
	dctx->file = NULL;
	dctx->tmpfile = NULL;
	dctx->format = format;
	if (header == NULL)
		dns_master_initrawheader(&dctx->header);
	else
		dctx->header = *header;

	switch (format) {
	case dns_masterformat_text:
		dctx->dumpsets = dump_rdatasets_text;
		break;
	case dns_masterformat_raw:
		dctx->dumpsets = dump_rdatasets_raw;
		break;
	case dns_masterformat_map:
		dctx->dumpsets = dump_rdatasets_map;
		break;
	default:
		INSIST(0);
		ISC_UNREACHABLE();
	}

	result = totext_ctx_init(style, &dctx->tctx);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "could not set master file style");
		goto cleanup;
	}

	isc_stdtime_get(&dctx->now);
	dns_db_attach(db, &dctx->db);

	dctx->do_date = dns_db_iscache(dctx->db);
	if (dctx->do_date) {
	    /*
	     * Adjust the date backwards by the serve-stale TTL, if any.
	     * This is so the TTL will be loaded correctly when next started.
	     */
	    (void)dns_db_getservestalettl(dctx->db,
					  &dctx->tctx.serve_stale_ttl);
	    dctx->now -= dctx->tctx.serve_stale_ttl;
	}

	if (dctx->format == dns_masterformat_text &&
	    (dctx->tctx.style.flags & DNS_STYLEFLAG_REL_OWNER) != 0) {
		options = DNS_DB_RELATIVENAMES;
	} else
		options = 0;
	result = dns_db_createiterator(dctx->db, options, &dctx->dbiter);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	result = isc_mutex_init(&dctx->lock);
	if (result != ISC_R_SUCCESS)
		goto cleanup;
	if (version != NULL)
		dns_db_attachversion(dctx->db, version, &dctx->version);
	else if (!dns_db_iscache(db))
		dns_db_currentversion(dctx->db, &dctx->version);
	isc_mem_attach(mctx, &dctx->mctx);
	dctx->references = 1;
	dctx->magic = DNS_DCTX_MAGIC;
	*dctxp = dctx;
	return (ISC_R_SUCCESS);

 cleanup:
	if (dctx->dbiter != NULL)
		dns_dbiterator_destroy(&dctx->dbiter);
	if (dctx->db != NULL)
		dns_db_detach(&dctx->db);
	if (dctx != NULL)
		isc_mem_put(mctx, dctx, sizeof(*dctx));
	return (result);
}

static isc_result_t
writeheader(dns_dumpctx_t *dctx) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_buffer_t buffer;
	char *bufmem;
	isc_region_t r;
	dns_masterrawheader_t rawheader;
	uint32_t rawversion, now32;

	bufmem = isc_mem_get(dctx->mctx, initial_buffer_length);
	if (bufmem == NULL)
		return (ISC_R_NOMEMORY);

	isc_buffer_init(&buffer, bufmem, initial_buffer_length);

	switch (dctx->format) {
	case dns_masterformat_text:
		/*
		 * If the database has cache semantics, output an
		 * RFC2540 $DATE directive so that the TTLs can be
		 * adjusted when it is reloaded.  For zones it is not
		 * really needed, and it would make the file
		 * incompatible with pre-RFC2540 software, so we omit
		 * it in the zone case.
		 */
		if (dctx->do_date) {
			fprintf(dctx->f,
				"; using a %u second stale ttl\n",
				dctx->tctx.serve_stale_ttl);
			result = dns_time32_totext(dctx->now, &buffer);
			RUNTIME_CHECK(result == ISC_R_SUCCESS);
			isc_buffer_usedregion(&buffer, &r);
			fprintf(dctx->f, "$DATE %.*s\n",
				(int) r.length, (char *) r.base);
		}
		break;
	case dns_masterformat_raw:
	case dns_masterformat_map:
		r.base = (unsigned char *)&rawheader;
		r.length = sizeof(rawheader);
		isc_buffer_region(&buffer, &r);
		now32 = dctx->now;
		rawversion = 1;
		if ((dctx->header.flags & DNS_MASTERRAW_COMPAT) != 0)
			rawversion = 0;

		isc_buffer_putuint32(&buffer, dctx->format);
		isc_buffer_putuint32(&buffer, rawversion);
		isc_buffer_putuint32(&buffer, now32);

		if (rawversion == 1) {
			isc_buffer_putuint32(&buffer, dctx->header.flags);
			isc_buffer_putuint32(&buffer,
					     dctx->header.sourceserial);
			isc_buffer_putuint32(&buffer, dctx->header.lastxfrin);
		}

		INSIST(isc_buffer_usedlength(&buffer) <= sizeof(rawheader));
		result = isc_stdio_write(buffer.base, 1,
					 isc_buffer_usedlength(&buffer),
					 dctx->f, NULL);
		if (result != ISC_R_SUCCESS)
			break;

		break;
	default:
		INSIST(0);
		ISC_UNREACHABLE();
	}

	isc_mem_put(dctx->mctx, buffer.base, buffer.length);
	return (result);
}

static isc_result_t
dumptostreaminc(dns_dumpctx_t *dctx) {
	isc_result_t result = ISC_R_SUCCESS;
	isc_buffer_t buffer;
	char *bufmem;
	dns_name_t *name;
	dns_fixedname_t fixname;
	unsigned int nodes;
	isc_time_t start;

	bufmem = isc_mem_get(dctx->mctx, initial_buffer_length);
	if (bufmem == NULL)
		return (ISC_R_NOMEMORY);

	isc_buffer_init(&buffer, bufmem, initial_buffer_length);

	name = dns_fixedname_initname(&fixname);

	if (dctx->first) {
		CHECK(writeheader(dctx));

		/*
		 * Fast format is not currently written incrementally,
		 * so we make the call to dns_db_serialize() here.
		 * If the database is anything other than an rbtdb,
		 * this should result in not implemented
		 */
		if (dctx->format == dns_masterformat_map) {
			result = dns_db_serialize(dctx->db, dctx->version,
						  dctx->f);
			goto cleanup;
		}

		result = dns_dbiterator_first(dctx->dbiter);
		if (result != ISC_R_SUCCESS && result != ISC_R_NOMORE)
			goto cleanup;

		dctx->first = false;
	} else
		result = ISC_R_SUCCESS;

	nodes = dctx->nodes;
	isc_time_now(&start);
	while (result == ISC_R_SUCCESS && (dctx->nodes == 0 || nodes--)) {
		dns_rdatasetiter_t *rdsiter = NULL;
		dns_dbnode_t *node = NULL;

		result = dns_dbiterator_current(dctx->dbiter, &node, name);
		if (result != ISC_R_SUCCESS && result != DNS_R_NEWORIGIN)
			break;
		if (result == DNS_R_NEWORIGIN) {
			dns_name_t *origin =
				dns_fixedname_name(&dctx->tctx.origin_fixname);
			result = dns_dbiterator_origin(dctx->dbiter, origin);
			RUNTIME_CHECK(result == ISC_R_SUCCESS);
			if ((dctx->tctx.style.flags &
			     DNS_STYLEFLAG_REL_DATA) != 0)
				dctx->tctx.origin = origin;
			dctx->tctx.neworigin = origin;
		}
		result = dns_db_allrdatasets(dctx->db, node, dctx->version,
					     dctx->now, &rdsiter);
		if (result != ISC_R_SUCCESS) {
			dns_db_detachnode(dctx->db, &node);
			goto cleanup;
		}
		result = (dctx->dumpsets)(dctx->mctx, name, rdsiter,
					  &dctx->tctx, &buffer, dctx->f);
		dns_rdatasetiter_destroy(&rdsiter);
		if (result != ISC_R_SUCCESS) {
			dns_db_detachnode(dctx->db, &node);
			goto cleanup;
		}
		dns_db_detachnode(dctx->db, &node);
		result = dns_dbiterator_next(dctx->dbiter);
	}

	/*
	 * Work out how many nodes can be written in the time between
	 * two requests to the nameserver.  Smooth the resulting number and
	 * use it as a estimate for the number of nodes to be written in the
	 * next iteration.
	 */
	if (dctx->nodes != 0 && result == ISC_R_SUCCESS) {
		unsigned int pps = dns_pps;	/* packets per second */
		unsigned int interval;
		uint64_t usecs;
		isc_time_t end;

		isc_time_now(&end);
		if (pps < 100)
			pps = 100;
		interval = 1000000 / pps;	/* interval in usecs */
		if (interval == 0)
			interval = 1;
		usecs = isc_time_microdiff(&end, &start);
		if (usecs == 0) {
			dctx->nodes = dctx->nodes * 2;
			if (dctx->nodes > 1000)
				dctx->nodes = 1000;
		} else {
			nodes = dctx->nodes * interval;
			nodes /= (unsigned int)usecs;
			if (nodes == 0)
				nodes = 1;
			else if (nodes > 1000)
				nodes = 1000;

			/* Smooth and assign. */
			dctx->nodes = (nodes + dctx->nodes * 7) / 8;

			isc_log_write(dns_lctx, ISC_LOGCATEGORY_GENERAL,
				      DNS_LOGMODULE_MASTERDUMP,
				      ISC_LOG_DEBUG(1),
				      "dumptostreaminc(%p) new nodes -> %d",
				      dctx, dctx->nodes);
		}
		result = DNS_R_CONTINUE;
	} else if (result == ISC_R_NOMORE)
		result = ISC_R_SUCCESS;
 cleanup:
	RUNTIME_CHECK(dns_dbiterator_pause(dctx->dbiter) == ISC_R_SUCCESS);
	isc_mem_put(dctx->mctx, buffer.base, buffer.length);
	return (result);
}

isc_result_t
dns_master_dumptostreaminc(isc_mem_t *mctx, dns_db_t *db,
			   dns_dbversion_t *version,
			   const dns_master_style_t *style,
			   FILE *f, isc_task_t *task,
			   dns_dumpdonefunc_t done, void *done_arg,
			   dns_dumpctx_t **dctxp)
{
	dns_dumpctx_t *dctx = NULL;
	isc_result_t result;

	REQUIRE(task != NULL);
	REQUIRE(f != NULL);
	REQUIRE(done != NULL);

	result = dumpctx_create(mctx, db, version, style, f, &dctx,
				dns_masterformat_text, NULL);
	if (result != ISC_R_SUCCESS)
		return (result);
	isc_task_attach(task, &dctx->task);
	dctx->done = done;
	dctx->done_arg = done_arg;
	dctx->nodes = 100;

	result = task_send(dctx);
	if (result == ISC_R_SUCCESS) {
		dns_dumpctx_attach(dctx, dctxp);
		return (DNS_R_CONTINUE);
	}

	dns_dumpctx_detach(&dctx);
	return (result);
}

isc_result_t
dns_master_dumptostream(isc_mem_t *mctx, dns_db_t *db,
			dns_dbversion_t *version,
			const dns_master_style_t *style,
			dns_masterformat_t format,
			dns_masterrawheader_t *header, FILE *f)
{
	dns_dumpctx_t *dctx = NULL;
	isc_result_t result;

	result = dumpctx_create(mctx, db, version, style, f, &dctx,
				format, header);
	if (result != ISC_R_SUCCESS)
		return (result);

	result = dumptostreaminc(dctx);
	INSIST(result != DNS_R_CONTINUE);
	dns_dumpctx_detach(&dctx);

	result = flushandsync(f, result, NULL);
	return (result);
}

static isc_result_t
opentmp(isc_mem_t *mctx, dns_masterformat_t format, const char *file,
	char **tempp, FILE **fp) {
	FILE *f = NULL;
	isc_result_t result;
	char *tempname = NULL;
	int tempnamelen;

	tempnamelen = strlen(file) + 20;
	tempname = isc_mem_allocate(mctx, tempnamelen);
	if (tempname == NULL)
		return (ISC_R_NOMEMORY);

	result = isc_file_mktemplate(file, tempname, tempnamelen);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	if (format == dns_masterformat_text)
		result = isc_file_openunique(tempname, &f);
	else
		result = isc_file_bopenunique(tempname, &f);
	if (result != ISC_R_SUCCESS) {
		isc_log_write(dns_lctx, ISC_LOGCATEGORY_GENERAL,
			      DNS_LOGMODULE_MASTERDUMP, ISC_LOG_ERROR,
			      "dumping master file: %s: open: %s",
			      tempname, isc_result_totext(result));
		goto cleanup;
	}
	*tempp = tempname;
	*fp = f;
	return (ISC_R_SUCCESS);

cleanup:
	isc_mem_free(mctx, tempname);
	return (result);
}

isc_result_t
dns_master_dumpinc(isc_mem_t *mctx, dns_db_t *db, dns_dbversion_t *version,
		   const dns_master_style_t *style, const char *filename,
		   isc_task_t *task, dns_dumpdonefunc_t done, void *done_arg,
		   dns_dumpctx_t **dctxp, dns_masterformat_t format,
		   dns_masterrawheader_t *header)
{
	FILE *f = NULL;
	isc_result_t result;
	char *tempname = NULL;
	char *file = NULL;
	dns_dumpctx_t *dctx = NULL;

	file = isc_mem_strdup(mctx, filename);
	if (file == NULL)
		return (ISC_R_NOMEMORY);

	result = opentmp(mctx, format, filename, &tempname, &f);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	result = dumpctx_create(mctx, db, version, style, f, &dctx,
				format, header);
	if (result != ISC_R_SUCCESS) {
		(void)isc_stdio_close(f);
		(void)isc_file_remove(tempname);
		goto cleanup;
	}

	isc_task_attach(task, &dctx->task);
	dctx->done = done;
	dctx->done_arg = done_arg;
	dctx->nodes = 100;
	dctx->file = file;
	file = NULL;
	dctx->tmpfile = tempname;
	tempname = NULL;

	result = task_send(dctx);
	if (result == ISC_R_SUCCESS) {
		dns_dumpctx_attach(dctx, dctxp);
		return (DNS_R_CONTINUE);
	}

 cleanup:
	if (dctx != NULL)
		dns_dumpctx_detach(&dctx);
	if (file != NULL)
		isc_mem_free(mctx, file);
	if (tempname != NULL)
		isc_mem_free(mctx, tempname);
	return (result);
}

isc_result_t
dns_master_dump(isc_mem_t *mctx, dns_db_t *db, dns_dbversion_t *version,
		const dns_master_style_t *style, const char *filename,
		dns_masterformat_t format, dns_masterrawheader_t *header)
{
	FILE *f = NULL;
	isc_result_t result;
	char *tempname;
	dns_dumpctx_t *dctx = NULL;

	result = opentmp(mctx, format, filename, &tempname, &f);
	if (result != ISC_R_SUCCESS)
		return (result);

	result = dumpctx_create(mctx, db, version, style, f, &dctx,
				format, header);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	result = dumptostreaminc(dctx);
	INSIST(result != DNS_R_CONTINUE);
	dns_dumpctx_detach(&dctx);

	result = closeandrename(f, result, tempname, filename);

 cleanup:
	isc_mem_free(mctx, tempname);
	return (result);
}

/*
 * Dump a database node into a master file.
 * XXX: this function assumes the text format.
 */
isc_result_t
dns_master_dumpnodetostream(isc_mem_t *mctx, dns_db_t *db,
			    dns_dbversion_t *version,
			    dns_dbnode_t *node, const dns_name_t *name,
			    const dns_master_style_t *style,
			    FILE *f)
{
	isc_result_t result;
	isc_buffer_t buffer;
	char *bufmem;
	isc_stdtime_t now;
	dns_totext_ctx_t ctx;
	dns_rdatasetiter_t *rdsiter = NULL;

	result = totext_ctx_init(style, &ctx);
	if (result != ISC_R_SUCCESS) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "could not set master file style");
		return (ISC_R_UNEXPECTED);
	}

	isc_stdtime_get(&now);

	bufmem = isc_mem_get(mctx, initial_buffer_length);
	if (bufmem == NULL)
		return (ISC_R_NOMEMORY);

	isc_buffer_init(&buffer, bufmem, initial_buffer_length);

	result = dns_db_allrdatasets(db, node, version, now, &rdsiter);
	if (result != ISC_R_SUCCESS)
		goto failure;
	result = dump_rdatasets_text(mctx, name, rdsiter, &ctx, &buffer, f);
	if (result != ISC_R_SUCCESS)
		goto failure;
	dns_rdatasetiter_destroy(&rdsiter);

	result = ISC_R_SUCCESS;

 failure:
	isc_mem_put(mctx, buffer.base, buffer.length);
	return (result);
}

isc_result_t
dns_master_dumpnode(isc_mem_t *mctx, dns_db_t *db, dns_dbversion_t *version,
		    dns_dbnode_t *node, const dns_name_t *name,
		    const dns_master_style_t *style, const char *filename)
{
	FILE *f = NULL;
	isc_result_t result;

	result = isc_stdio_open(filename, "w", &f);
	if (result != ISC_R_SUCCESS) {
		isc_log_write(dns_lctx, ISC_LOGCATEGORY_GENERAL,
			      DNS_LOGMODULE_MASTERDUMP, ISC_LOG_ERROR,
			      "dumping node to file: %s: open: %s", filename,
			      isc_result_totext(result));
		return (ISC_R_UNEXPECTED);
	}

	result = dns_master_dumpnodetostream(mctx, db, version, node, name,
					     style, f);
	if (result != ISC_R_SUCCESS) {
		isc_log_write(dns_lctx, ISC_LOGCATEGORY_GENERAL,
			      DNS_LOGMODULE_MASTERDUMP, ISC_LOG_ERROR,
			      "dumping master file: %s: dump: %s", filename,
			      isc_result_totext(result));
		(void)isc_stdio_close(f);
		return (ISC_R_UNEXPECTED);
	}

	result = isc_stdio_close(f);
	if (result != ISC_R_SUCCESS) {
		isc_log_write(dns_lctx, ISC_LOGCATEGORY_GENERAL,
			      DNS_LOGMODULE_MASTERDUMP, ISC_LOG_ERROR,
			      "dumping master file: %s: close: %s", filename,
			      isc_result_totext(result));
		return (ISC_R_UNEXPECTED);
	}

	return (result);
}

dns_masterstyle_flags_t
dns_master_styleflags(const dns_master_style_t *style) {
	REQUIRE(style != NULL);
	return (style->flags);
}

isc_result_t
dns_master_stylecreate(dns_master_style_t **stylep,
		       dns_masterstyle_flags_t flags,
		       unsigned int ttl_column, unsigned int class_column,
		       unsigned int type_column, unsigned int rdata_column,
		       unsigned int line_length, unsigned int tab_width,
		       unsigned int split_width, isc_mem_t *mctx)
{
	dns_master_style_t *style;

	REQUIRE(stylep != NULL && *stylep == NULL);
	style = isc_mem_get(mctx, sizeof(*style));
	if (style == NULL)
		return (ISC_R_NOMEMORY);

	style->flags = flags;
	style->ttl_column = ttl_column;
	style->class_column = class_column;
	style->type_column = type_column;
	style->rdata_column = rdata_column;
	style->line_length = line_length;
	style->tab_width = tab_width;
	style->split_width = split_width;
	*stylep = style;
	return (ISC_R_SUCCESS);
}

void
dns_master_styledestroy(dns_master_style_t **stylep, isc_mem_t *mctx) {
	dns_master_style_t *style;

	REQUIRE(stylep != NULL && *stylep != NULL);
	style = *stylep;
	*stylep = NULL;
	isc_mem_put(mctx, style, sizeof(*style));
}
