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

#include <stdbool.h>
#include <stdlib.h>
#include <inttypes.h>

#include <isc/app.h>
#include <isc/commandline.h>
#include <isc/dir.h>
#include <isc/entropy.h>
#include <isc/hash.h>
#include <isc/log.h>
#include <isc/mem.h>
#include <isc/print.h>
#include <isc/socket.h>
#include <isc/string.h>
#include <isc/task.h>
#include <isc/timer.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/fixedname.h>
#include <dns/log.h>
#include <dns/master.h>
#include <dns/masterdump.h>
#include <dns/name.h>
#include <dns/rdataclass.h>
#include <dns/rdataset.h>
#include <dns/result.h>
#include <dns/types.h>
#include <dns/zone.h>

#include "check-tool.h"

static int quiet = 0;
static isc_mem_t *mctx = NULL;
static isc_entropy_t *ectx = NULL;
dns_zone_t *zone = NULL;
dns_zonetype_t zonetype = dns_zone_master;
static int dumpzone = 0;
static const char *output_filename;
static const char *prog_name = NULL;
static const dns_master_style_t *outputstyle = NULL;
static enum { progmode_check, progmode_compile } progmode;

#define ERRRET(result, function) \
	do { \
		if (result != ISC_R_SUCCESS) { \
			if (!quiet) \
				fprintf(stderr, "%s() returned %s\n", \
					function, dns_result_totext(result)); \
			return (result); \
		} \
	} while (0)

ISC_PLATFORM_NORETURN_PRE static void
usage(void) ISC_PLATFORM_NORETURN_POST;

static void
usage(void) {
	fprintf(stderr,
		"usage: %s [-djqvD] [-c class] "
		"[-f inputformat] [-F outputformat] [-J filename] "
		"[-t directory] [-w directory] [-k (ignore|warn|fail)] "
		"[-n (ignore|warn|fail)] [-m (ignore|warn|fail)] "
		"[-r (ignore|warn|fail)] "
		"[-i (full|full-sibling|local|local-sibling|none)] "
		"[-M (ignore|warn|fail)] [-S (ignore|warn|fail)] "
		"[-W (ignore|warn)] "
		"%s zonename filename\n",
		prog_name,
		progmode == progmode_check ? "[-o filename]" : "-o filename");
	exit(1);
}

static void
destroy(void) {
	if (zone != NULL)
		dns_zone_detach(&zone);
	dns_name_destroy();
}

/*% main processing routine */
int
main(int argc, char **argv) {
	int c;
	char *origin = NULL;
	char *filename = NULL;
	isc_log_t *lctx = NULL;
	isc_result_t result;
	char classname_in[] = "IN";
	char *classname = classname_in;
	const char *workdir = NULL;
	const char *inputformatstr = NULL;
	const char *outputformatstr = NULL;
	dns_masterformat_t inputformat = dns_masterformat_text;
	dns_masterformat_t outputformat = dns_masterformat_text;
	dns_masterrawheader_t header;
	uint32_t rawversion = 1, serialnum = 0;
	dns_ttl_t maxttl = 0;
	bool snset = false;
	bool logdump = false;
	FILE *errout = stdout;
	char *endp;

	/*
	 * Uncomment the following line if memory debugging is needed:
	 * isc_mem_debugging |= ISC_MEM_DEBUGRECORD;
	 */

	outputstyle = &dns_master_style_full;

	prog_name = strrchr(argv[0], '/');
	if (prog_name == NULL)
		prog_name = strrchr(argv[0], '\\');
	if (prog_name != NULL)
		prog_name++;
	else
		prog_name = argv[0];
	/*
	 * Libtool doesn't preserve the program name prior to final
	 * installation.  Remove the libtool prefix ("lt-").
	 */
	if (strncmp(prog_name, "lt-", 3) == 0)
		prog_name += 3;

#define PROGCMP(X) \
	(strcasecmp(prog_name, X) == 0 || strcasecmp(prog_name, X ".exe") == 0)

	if (PROGCMP("named-checkzone"))
		progmode = progmode_check;
	else if (PROGCMP("named-compilezone"))
		progmode = progmode_compile;
	else
		INSIST(0);

	/* Compilation specific defaults */
	if (progmode == progmode_compile) {
		zone_options |= (DNS_ZONEOPT_CHECKNS |
				 DNS_ZONEOPT_FATALNS |
				 DNS_ZONEOPT_CHECKSPF |
				 DNS_ZONEOPT_CHECKDUPRR |
				 DNS_ZONEOPT_CHECKNAMES |
				 DNS_ZONEOPT_CHECKNAMESFAIL |
				 DNS_ZONEOPT_CHECKWILDCARD);
	} else
		zone_options |= (DNS_ZONEOPT_CHECKDUPRR |
				 DNS_ZONEOPT_CHECKSPF);

#define ARGCMP(X) (strcmp(isc_commandline_argument, X) == 0)

	isc_commandline_errprint = false;

	while ((c = isc_commandline_parse(argc, argv,
			       "c:df:hi:jJ:k:L:l:m:n:qr:s:t:o:vw:DF:M:S:T:W:"))
	       != EOF) {
		switch (c) {
		case 'c':
			classname = isc_commandline_argument;
			break;

		case 'd':
			debug++;
			break;

		case 'i':
			if (ARGCMP("full")) {
				zone_options |= DNS_ZONEOPT_CHECKINTEGRITY |
						DNS_ZONEOPT_CHECKSIBLING;
				docheckmx = true;
				docheckns = true;
				dochecksrv = true;
			} else if (ARGCMP("full-sibling")) {
				zone_options |= DNS_ZONEOPT_CHECKINTEGRITY;
				zone_options &= ~DNS_ZONEOPT_CHECKSIBLING;
				docheckmx = true;
				docheckns = true;
				dochecksrv = true;
			} else if (ARGCMP("local")) {
				zone_options |= DNS_ZONEOPT_CHECKINTEGRITY;
				zone_options |= DNS_ZONEOPT_CHECKSIBLING;
				docheckmx = false;
				docheckns = false;
				dochecksrv = false;
			} else if (ARGCMP("local-sibling")) {
				zone_options |= DNS_ZONEOPT_CHECKINTEGRITY;
				zone_options &= ~DNS_ZONEOPT_CHECKSIBLING;
				docheckmx = false;
				docheckns = false;
				dochecksrv = false;
			} else if (ARGCMP("none")) {
				zone_options &= ~DNS_ZONEOPT_CHECKINTEGRITY;
				zone_options &= ~DNS_ZONEOPT_CHECKSIBLING;
				docheckmx = false;
				docheckns = false;
				dochecksrv = false;
			} else {
				fprintf(stderr, "invalid argument to -i: %s\n",
					isc_commandline_argument);
				exit(1);
			}
			break;

		case 'f':
			inputformatstr = isc_commandline_argument;
			break;

		case 'F':
			outputformatstr = isc_commandline_argument;
			break;

		case 'j':
			nomerge = false;
			break;

		case 'J':
			journal = isc_commandline_argument;
			nomerge = false;
			break;

		case 'k':
			if (ARGCMP("warn")) {
				zone_options |= DNS_ZONEOPT_CHECKNAMES;
				zone_options &= ~DNS_ZONEOPT_CHECKNAMESFAIL;
			} else if (ARGCMP("fail")) {
				zone_options |= DNS_ZONEOPT_CHECKNAMES |
						DNS_ZONEOPT_CHECKNAMESFAIL;
			} else if (ARGCMP("ignore")) {
				zone_options &= ~(DNS_ZONEOPT_CHECKNAMES |
						  DNS_ZONEOPT_CHECKNAMESFAIL);
			} else {
				fprintf(stderr, "invalid argument to -k: %s\n",
					isc_commandline_argument);
				exit(1);
			}
			break;

		case 'L':
			snset = true;
			endp = NULL;
			serialnum = strtol(isc_commandline_argument, &endp, 0);
			if (*endp != '\0') {
				fprintf(stderr, "source serial number "
						"must be numeric");
				exit(1);
			}
			break;

		case 'l':
			zone_options2 |= DNS_ZONEOPT2_CHECKTTL;
			endp = NULL;
			maxttl = strtol(isc_commandline_argument, &endp, 0);
			if (*endp != '\0') {
				fprintf(stderr, "maximum TTL "
						"must be numeric");
				exit(1);
			}
			break;


		case 'n':
			if (ARGCMP("ignore")) {
				zone_options &= ~(DNS_ZONEOPT_CHECKNS|
						  DNS_ZONEOPT_FATALNS);
			} else if (ARGCMP("warn")) {
				zone_options |= DNS_ZONEOPT_CHECKNS;
				zone_options &= ~DNS_ZONEOPT_FATALNS;
			} else if (ARGCMP("fail")) {
				zone_options |= DNS_ZONEOPT_CHECKNS|
						DNS_ZONEOPT_FATALNS;
			} else {
				fprintf(stderr, "invalid argument to -n: %s\n",
					isc_commandline_argument);
				exit(1);
			}
			break;

		case 'm':
			if (ARGCMP("warn")) {
				zone_options |= DNS_ZONEOPT_CHECKMX;
				zone_options &= ~DNS_ZONEOPT_CHECKMXFAIL;
			} else if (ARGCMP("fail")) {
				zone_options |= DNS_ZONEOPT_CHECKMX |
						DNS_ZONEOPT_CHECKMXFAIL;
			} else if (ARGCMP("ignore")) {
				zone_options &= ~(DNS_ZONEOPT_CHECKMX |
						  DNS_ZONEOPT_CHECKMXFAIL);
			} else {
				fprintf(stderr, "invalid argument to -m: %s\n",
					isc_commandline_argument);
				exit(1);
			}
			break;

		case 'o':
			output_filename = isc_commandline_argument;
			break;

		case 'q':
			quiet++;
			break;

		case 'r':
			if (ARGCMP("warn")) {
				zone_options |= DNS_ZONEOPT_CHECKDUPRR;
				zone_options &= ~DNS_ZONEOPT_CHECKDUPRRFAIL;
			} else if (ARGCMP("fail")) {
				zone_options |= DNS_ZONEOPT_CHECKDUPRR |
						DNS_ZONEOPT_CHECKDUPRRFAIL;
			} else if (ARGCMP("ignore")) {
				zone_options &= ~(DNS_ZONEOPT_CHECKDUPRR |
						  DNS_ZONEOPT_CHECKDUPRRFAIL);
			} else {
				fprintf(stderr, "invalid argument to -r: %s\n",
					isc_commandline_argument);
				exit(1);
			}
			break;

		case 's':
			if (ARGCMP("full"))
				outputstyle = &dns_master_style_full;
			else if (ARGCMP("relative")) {
				outputstyle = &dns_master_style_default;
			} else {
				fprintf(stderr,
					"unknown or unsupported style: %s\n",
					isc_commandline_argument);
				exit(1);
			}
			break;

		case 't':
			result = isc_dir_chroot(isc_commandline_argument);
			if (result != ISC_R_SUCCESS) {
				fprintf(stderr, "isc_dir_chroot: %s: %s\n",
					isc_commandline_argument,
					isc_result_totext(result));
				exit(1);
			}
			break;

		case 'v':
			printf(VERSION "\n");
			exit(0);

		case 'w':
			workdir = isc_commandline_argument;
			break;

		case 'D':
			dumpzone++;
			break;

		case 'M':
			if (ARGCMP("fail")) {
				zone_options &= ~DNS_ZONEOPT_WARNMXCNAME;
				zone_options &= ~DNS_ZONEOPT_IGNOREMXCNAME;
			} else if (ARGCMP("warn")) {
				zone_options |= DNS_ZONEOPT_WARNMXCNAME;
				zone_options &= ~DNS_ZONEOPT_IGNOREMXCNAME;
			} else if (ARGCMP("ignore")) {
				zone_options |= DNS_ZONEOPT_WARNMXCNAME;
				zone_options |= DNS_ZONEOPT_IGNOREMXCNAME;
			} else {
				fprintf(stderr, "invalid argument to -M: %s\n",
					isc_commandline_argument);
				exit(1);
			}
			break;

		case 'S':
			if (ARGCMP("fail")) {
				zone_options &= ~DNS_ZONEOPT_WARNSRVCNAME;
				zone_options &= ~DNS_ZONEOPT_IGNORESRVCNAME;
			} else if (ARGCMP("warn")) {
				zone_options |= DNS_ZONEOPT_WARNSRVCNAME;
				zone_options &= ~DNS_ZONEOPT_IGNORESRVCNAME;
			} else if (ARGCMP("ignore")) {
				zone_options |= DNS_ZONEOPT_WARNSRVCNAME;
				zone_options |= DNS_ZONEOPT_IGNORESRVCNAME;
			} else {
				fprintf(stderr, "invalid argument to -S: %s\n",
					isc_commandline_argument);
				exit(1);
			}
			break;

		case 'T':
			if (ARGCMP("warn")) {
				zone_options |= DNS_ZONEOPT_CHECKSPF;
			} else if (ARGCMP("ignore")) {
				zone_options &= ~DNS_ZONEOPT_CHECKSPF;
			} else {
				fprintf(stderr, "invalid argument to -T: %s\n",
					isc_commandline_argument);
				exit(1);
			}
			break;

		case 'W':
			if (ARGCMP("warn"))
				zone_options |= DNS_ZONEOPT_CHECKWILDCARD;
			else if (ARGCMP("ignore"))
				zone_options &= ~DNS_ZONEOPT_CHECKWILDCARD;
			break;

		case '?':
			if (isc_commandline_option != '?')
				fprintf(stderr, "%s: invalid argument -%c\n",
					prog_name, isc_commandline_option);
			/* FALLTHROUGH */
		case 'h':
			usage();

		default:
			fprintf(stderr, "%s: unhandled option -%c\n",
				prog_name, isc_commandline_option);
			exit(1);
		}
	}

	if (workdir != NULL) {
		result = isc_dir_chdir(workdir);
		if (result != ISC_R_SUCCESS) {
			fprintf(stderr, "isc_dir_chdir: %s: %s\n",
				workdir, isc_result_totext(result));
			exit(1);
		}
	}

	if (inputformatstr != NULL) {
		if (strcasecmp(inputformatstr, "text") == 0)
			inputformat = dns_masterformat_text;
		else if (strcasecmp(inputformatstr, "raw") == 0)
			inputformat = dns_masterformat_raw;
		else if (strncasecmp(inputformatstr, "raw=", 4) == 0) {
			inputformat = dns_masterformat_raw;
			fprintf(stderr,
				"WARNING: input format raw, version ignored\n");
		} else if (strcasecmp(inputformatstr, "map") == 0) {
			inputformat = dns_masterformat_map;
		} else {
			fprintf(stderr, "unknown file format: %s\n",
			    inputformatstr);
			exit(1);
		}
	}

	if (outputformatstr != NULL) {
		if (strcasecmp(outputformatstr, "text") == 0) {
			outputformat = dns_masterformat_text;
		} else if (strcasecmp(outputformatstr, "raw") == 0) {
			outputformat = dns_masterformat_raw;
		} else if (strncasecmp(outputformatstr, "raw=", 4) == 0) {
			char *end;

			outputformat = dns_masterformat_raw;
			rawversion = strtol(outputformatstr + 4, &end, 10);
			if (end == outputformatstr + 4 || *end != '\0' ||
			    rawversion > 1U) {
				fprintf(stderr,
					"unknown raw format version\n");
				exit(1);
			}
		} else if (strcasecmp(outputformatstr, "map") == 0) {
			outputformat = dns_masterformat_map;
		} else {
			fprintf(stderr, "unknown file format: %s\n",
				outputformatstr);
			exit(1);
		}
	}

	if (progmode == progmode_compile) {
		dumpzone = 1;	/* always dump */
		logdump = !quiet;
		if (output_filename == NULL) {
			fprintf(stderr,
				"output file required, but not specified\n");
			usage();
		}
	}

	if (output_filename != NULL)
		dumpzone = 1;

	/*
	 * If we are outputing to stdout then send the informational
	 * output to stderr.
	 */
	if (dumpzone &&
	    (output_filename == NULL ||
	     strcmp(output_filename, "-") == 0 ||
	     strcmp(output_filename, "/dev/fd/1") == 0 ||
	     strcmp(output_filename, "/dev/stdout") == 0)) {
		errout = stderr;
		logdump = false;
	}

	if (isc_commandline_index + 2 != argc)
		usage();

#ifdef _WIN32
	InitSockets();
#endif

	RUNTIME_CHECK(isc_mem_create(0, 0, &mctx) == ISC_R_SUCCESS);
	if (!quiet)
		RUNTIME_CHECK(setup_logging(mctx, errout, &lctx)
			      == ISC_R_SUCCESS);
	RUNTIME_CHECK(isc_entropy_create(mctx, &ectx) == ISC_R_SUCCESS);
	RUNTIME_CHECK(isc_hash_create(mctx, ectx, DNS_NAME_MAXWIRE)
		      == ISC_R_SUCCESS);

	dns_result_register();

	origin = argv[isc_commandline_index++];
	filename = argv[isc_commandline_index++];
	result = load_zone(mctx, origin, filename, inputformat, classname,
			   maxttl, &zone);

	if (snset) {
		dns_master_initrawheader(&header);
		header.flags = DNS_MASTERRAW_SOURCESERIALSET;
		header.sourceserial = serialnum;
		dns_zone_setrawdata(zone, &header);
	}

	if (result == ISC_R_SUCCESS && dumpzone) {
		if (logdump) {
			fprintf(errout, "dump zone to %s...", output_filename);
			fflush(errout);
		}
		result = dump_zone(origin, zone, output_filename,
				   outputformat, outputstyle, rawversion);
		if (logdump)
			fprintf(errout, "done\n");
	}

	if (!quiet && result == ISC_R_SUCCESS)
		fprintf(errout, "OK\n");
	destroy();
	if (lctx != NULL)
		isc_log_destroy(&lctx);
	isc_hash_destroy();
	isc_entropy_detach(&ectx);
	isc_mem_destroy(&mctx);
#ifdef _WIN32
	DestroySockets();
#endif
	return ((result == ISC_R_SUCCESS) ? 0 : 1);
}
