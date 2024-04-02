/* Public Domain, feel free to use this, as you wish.
   rilysh <horizon@quicknq.anonaddy.me> */

#define _GNU_SOURCE

/* Generic includes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <curl/curl.h>
#include <parson.h>

/* General macros. */
#define AUR_BASE_URL            "https://aur.archlinux.org"
#define AUR_SEARCH_URL          "https://aur.archlinux.org/rpc/v5/search"
#define AUR_INFO_URL            "https://aur.archlinux.org/rpc/v5/info"
#define AUR_CGIT_PATH           "cgit/aur.git/snapshot"
#define DEFAULT_TAR_PATH        "/usr/bin/tar"
#define ALT_TAR_PATH            "/bin/tar"
#define DEFAULT_MAKEPKG_PATH    "/usr/bin/makepkg"
#define DEFAULT_OS_RELEASE      "/etc/os-release"

/* Color macros. */
#define COLOR_BLUE     "\x1b[1;34m"
#define COLOR_WHITE    "\x1b[1;37m"
#define COLOR_PURPLE   "\x1b[0;35m"
#define COLOR_LCYAN    "\x1b[0;36m"
#define COLOR_BRED     "\x1b[1;31m"
#define COLOR_LGREEN   "\x1b[0;32m"
#define COLOR_BGREEN   "\x1b[1;32m"
#define COLOR_END      "\x1b[0m"
#define UNDERLINE      "\x1b[4m"

/* Array size. */
#define ARRAY_SIZE(x)           (sizeof(x) / sizeof(x[0]))

/* To hold curl's response. */
struct curl_memory {
        char *resp;
	size_t nsz;
	size_t bt;
};

/* Main AUR package structure.
   Holds various types of information. */
struct aur_pkg {
	const char *name;
        const char *description;
        uint32_t id;
        time_t first_sub;
	time_t last_mod;
	const char *maintainer;
	uint32_t numvotes;
	time_t outdated;
        double popularity;
	const char *url;
	const char *url_path;
	const char *url_base;
	const char *version;
};

/* AUR package information structure. */
struct aur_pkg_info {
	const char *name;
	const char *description;
	const char *url;
	const char *version;
	char *depends;
	char *licenses;
	char *keywords;
	char *optdeps;
        uint32_t num_votes;
        time_t first_sub;
	time_t last_mod;
        time_t outdated;
	double popularity;
};

/* Options structure. */
struct arg_opts {
	int c;
	int opt_idx;
	int is_search;
	int is_info;
	int is_get;
	int is_colors;
	int is_help;
};

/* Format the URL for AUR_SEARCH_URL. */
static char *format_simple_url(const char *name)
{
	char *p;
	size_t sz;

	/* Length of the AUR_SEARCH_URL, 1 for the "/",
	   and last one for the null terminator. */
	sz = strlen(name) + (size_t)41;
	p = calloc(sz, sizeof(char));
	if (p == NULL)
		err(EXIT_FAILURE, "calloc()");

	snprintf(p, sz, "%s/%s", AUR_SEARCH_URL, name);
	return (p);
}

/* Get the basename from a path or URL. */
static char *base_name(const char *str)
{
	char *base;

	base = strrchr(str, '/');
	if (base == NULL)
		return (NULL);
	else
		/* Consume the leading '/'. */
		return (base + 1);
}

/* Curl's write callback, used to store the response buffer. */
static size_t curl_write_cb(void *data, size_t sz, size_t nmb, void *usrp)
{
	size_t rsz;
	struct curl_memory *cm;
	char *rp;

	rsz = sz * nmb;
	cm = (struct curl_memory *)usrp;

        rp = realloc(cm->resp, cm->nsz + rsz + (size_t)1);
	if (rp == NULL) {
		free(cm->resp);
		err(EXIT_FAILURE, "realloc()");
	}

	cm->resp = rp;
	memcpy(cm->resp + cm->nsz, data, rsz);
	cm->nsz += rsz;
	cm->resp[cm->nsz] = '\0';

	return (rsz);
}

/* Decompress .tar.gz archives, by executing the general "tar" command.
   Note that it doesn't check whether you've or not the gunzip command. */
static void targz_decompress_archive(char *pkg)
{
        pid_t pid;
	int ret;
        char *path;

	ret = access(DEFAULT_TAR_PATH, F_OK);
	if (ret == -1) {
		if (errno == ENOENT) {
			ret = access(ALT_TAR_PATH, F_OK);
			if (ret == -1) {
				if (errno == ENOENT) {
					errx(EXIT_FAILURE,
					     "access(): 'tar' is not installed.");
				} else {
					/* access() error. */
					err(EXIT_FAILURE, "access()");
				}
			} else {
				/* If access() is not -1. */
				path = ALT_TAR_PATH;
			}
		} else {
			/* Parent access() error. */
			err(EXIT_FAILURE, "access()");
		}
	} else {
		/* If parent access() is not -1. */
		path = DEFAULT_TAR_PATH;
	}

	pid = fork();
	if (pid == (pid_t)-1)
	        err(EXIT_FAILURE, "fork()");

	if (pid == (pid_t)0) {
		ret = execl(path, "tar", "xf", pkg, (char *)NULL);
		if (ret == -1)
			_exit(127);
	}

	while (waitpid(pid, NULL, 0) < 0)
		;
}

/* Read 2-bytes (magic number) from the provided file,
   to identify whether the archive/file is a valid gzipped
   file or not. */
static int likely_targz_magic_sig(const char *file)
{
	FILE *fp;
	uint8_t mag[2];
        int ret;

	fp = fopen(file, "rb");
	if (fp == NULL)
		err(EXIT_FAILURE, "fopen()");

        fread(mag, (size_t)1, ARRAY_SIZE(mag), fp);
	fclose(fp);

	/* Match for 8b1f. */
	ret = mag[0] == 0x1f && mag[1] == 0x8b;
	return (ret);
}

/* Check whether the system is Arch GNU/Linux or not.
   Note that this check is not exhaustive. It only checks
   whether os-release file does contain the word "Arch.*"
   or matching "arch linux" word. If os-release file does
   not exists, I'll assume that you're not using Arch GNU/Linux
   as Arch do package a os-release file. */
static int likely_running_arch_gnu(void)
{
	FILE *fp;
	long fsz, st;
	char *p;

	/* Open the os-release file. */
	fp = fopen(DEFAULT_OS_RELEASE, "r");
	if (fp == NULL)
		return (0);

	fseek(fp, (long)1, SEEK_END);
        fsz = ftell(fp);
	fseek(fp, (long)0, SEEK_SET);

	/* This can't be... but a check won't be that bad. */
	if (fsz == 0)
		err(EXIT_FAILURE, "ftell()");

	p = calloc((size_t)fsz, sizeof(char));
	if (p == NULL)
        	err(EXIT_FAILURE, "calloc()");

        fread(p, (size_t)1, (size_t)fsz, fp);
	fclose(fp);

	/* Check for specific strings, containing the
	   word, "arch linux" in their respective word
	   case. */
	st = 0;
	if (strstr(p, "Arch Linux"))
		st = 1;
	else if (strstr(p, "arch"))
		st = 1;
        else if (strstr(p, "https://archlinux.org"))
		st = 1;

	/* If st is 0, then it is not an Arch GNU/Linux
	   system, otherwise, yes, it is. */
	free(p);
	return (st);
}

/* Print a warning message, if the system isn't Arch GNU/Linux. */
static void print_warn_not_arch_gnu(int enable_colors)
{
	if (enable_colors) {
		fputs(COLOR_BLUE":: "COLOR_END, stderr);
	        fputs(COLOR_BRED"Warning: "COLOR_END
		      COLOR_WHITE"You are not running Arch GNU/Linux. "
		      "'makepkg' will be disabled.\n"COLOR_END, stderr);
	} else {
		fputs(":: ", stderr);
	        fputs("Warning: "
		      "You are not running Arch GNU/Linux. "
		      "'makepkg' will be disabled.\n", stderr);
	}
}

/* Run makepkg. */
static void makepkg_and_install(const char *dir)
{
	pid_t pid;
	int ret;

	/* Check whether you're using Arch GNU/Linux or not. */
	if (likely_running_arch_gnu() == 0)
		errx(EXIT_FAILURE,
		     "You are not running Arch GNU/Linux. "
		     "So I cannot run 'makepkg' here.");

	ret = access(DEFAULT_MAKEPKG_PATH, F_OK);
	if (ret == -1) {
		if (errno == ENOENT)
			errx(EXIT_FAILURE,
			     "access(): 'makepkg' is not installed.");
		else
			err(EXIT_FAILURE, "access()");
	}

	pid = fork();
	if (pid == (pid_t)-1)
	        err(EXIT_FAILURE, "fork()");

	if (pid == (pid_t)0) {
	        if (chdir(dir) == -1)
			err(EXIT_FAILURE, "chdir()");

		ret = execl(DEFAULT_MAKEPKG_PATH, "makepkg", "-si",
			    (char *)NULL);
		if (ret == -1)
			_exit(127);
	}

	/* Wait till it's done. */
        while (waitpid(pid, NULL, 0) < 0)
		;
}

/* Do curl request to search for a specific package. */
static char *search_for_pkg(const char *pkg)
{
	CURL *curl;
	CURLcode ret;
	char *fmt;
	struct curl_memory cm;

	/* Zero-fill the curl_memory structure. */
	memset(&cm, '\0', sizeof(struct curl_memory));
        curl = curl_easy_init();
	if (curl == NULL)
		err(EXIT_FAILURE, "curl_easy_init()");

	fmt = format_simple_url(pkg);
	curl_easy_setopt(curl, CURLOPT_URL, fmt);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, (long)1);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&cm);

	ret = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	curl_global_cleanup();
	free(fmt);

	if (ret != CURLE_OK)
	        err(EXIT_FAILURE, "curl_easy_perform()");

	/* Return the response buffer. */
	return (cm.resp);
}

/* Using curl, download a file from the URL. */
static void download_from_url(const char *name, const char *url,
			      long show_progress)
{
	FILE *fp;
	CURL *curl;
	CURLcode ret;

	fp = fopen(name, "wb");
	if (fp == NULL)
		err(EXIT_FAILURE, "fopen()");

	curl = curl_easy_init();
	if (curl == NULL)
		err(EXIT_FAILURE, "curl_easy_init()");

        curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, show_progress);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, (long)1);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)50);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

	ret = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	curl_global_cleanup();
	fclose(fp);

	if (ret != CURLE_OK)
        	err(EXIT_FAILURE, "curl_easy_perform()");
}

/* Quicksort comparision function. */
static int sort_compare(const void *a, const void *b)
{
	const JSON_Object *sa, *sb;
        uint64_t vsa, vsb;

	sa = *(const JSON_Object **)a;
	sb = *(const JSON_Object **)b;

	/* Number of votes. */
	vsa = (uint64_t)json_object_get_number(sa, "NumVotes");
	vsb = (uint64_t)json_object_get_number(sb, "NumVotes");

	/* TODO: See Ant's answer. */
	return ((vsa > vsb) - (vsa < vsb));
}

/* Pretty print the time. */
static char *pretty_time(time_t time)
{
        struct tm t, *lt;
	char *tp;

	/* Get the localtime of that timestamp. */
	lt = localtime_r(&time, &t);
	if (lt == NULL)
		err(EXIT_FAILURE, "localtime_r()");

	tp = calloc((size_t)16, sizeof(char));
	if (tp == NULL)
		err(EXIT_FAILURE, "calloc()");

	strftime(tp, (size_t)16, "%Y-%m-%d", &t);
	return (tp);
}

/* Safely use strtoul (unsigned long). */
static uintptr_t safe_atoul(const char *str)
{
	uintptr_t ret;

	ret = strtoul(str, (char **)NULL, 10);
	return (ret);
}

/* Pretty print all search results and add them to the aur_pkg structure. */
static void print_search_results(const char *json, int enable_colors)
{
	size_t i, j, lcount, usz;
	JSON_Value *jsch;
	JSON_Array *jarr;
	JSON_Object *jobj, **jobjs;
        char *date, *p, *k, *base;
	char vstdin[256];
        struct aur_pkg *aur;
	int fpkg;
	size_t inum, didx;

	jsch = json_parse_string(json);
	jobj = json_object(jsch);
	jarr = json_object_get_array(jobj, "results");
        lcount = (size_t)json_object_get_number(jobj, "resultcount");

	if (lcount == (size_t)0) {
		fputs("error: no package results were found.\n",
		      stderr);
		return;
	}

	/* JSON objects structure. */
	jobjs = calloc(sizeof(struct JSON_Object *) * lcount,
		       sizeof(char));
	if (jobjs == NULL)
		err(EXIT_FAILURE, "calloc()");

	/* aur_pkg structure. */
	aur = calloc(sizeof(*aur) * lcount, sizeof(struct aur_pkg *));
	if (aur == NULL)
		err(EXIT_FAILURE, "calloc()");

	/* Set the each object to jobjs. */
	for (i = 0; i < lcount; i++)
		jobjs[i] = json_array_get_object(jarr, i);

	/* Sort the JSON structure, according whoever got the most
	   votes, by swapping the pointers. */
        qsort(jobjs, lcount, sizeof(JSON_Object *), sort_compare);

	/* Now sorted, so let's print each objects out. */
	for (i = 0; i < lcount; i++) {
		aur[i].name = json_object_get_string(jobjs[i], "Name");
		aur[i].description = json_object_get_string(jobjs[i], "Description");
		if (aur[i].description == NULL)
			aur[i].description = "no description was specified";
	        aur[i].version = json_object_get_string(jobjs[i], "Version");
		if (aur[i].version == NULL)
			aur[i].version = "unknown";
		aur[i].numvotes = (uint32_t)json_object_get_number(jobjs[i], "NumVotes");
		aur[i].popularity = json_object_get_number(jobjs[i], "Popularity");
		aur[i].outdated = (time_t)json_object_get_number(jobjs[i], "OutOfDate");
		/* If there are no maintainer, then the package is considerd orphaned. */
		aur[i].maintainer = json_object_get_string(jobjs[i], "Maintainer");

		/* Apparently, package path may not be correct when using URLPath.
		   Either because it's outdated or not updated in the AUR repository.
		   To "fix" that use "PackageBase" as the archive name. */
		aur[i].url_path = json_object_get_string(jobjs[i], "URLPath");
		aur[i].url_base = json_object_get_string(jobjs[i], "PackageBase");
	}

	/* Show colored output, if colors are enabled. */
	if (enable_colors) {
		for (i = 0, j = 1; i < lcount; i++, j++) {
			fprintf(stdout, COLOR_PURPLE"%zu "
				COLOR_BLUE"aur"COLOR_END"/", j);
			fprintf(stdout, COLOR_WHITE"%s"COLOR_END
				" "COLOR_BGREEN"(%s)"COLOR_END,
				aur[i].name, aur[i].version);
			fprintf(stdout, COLOR_WHITE" (+%u %.2lf%%)"COLOR_END,
				aur[i].numvotes, aur[i].popularity);

			/* Is there no maintainer? Package must be orphaned. */
			if (aur[i].maintainer == NULL)
			        fputs(COLOR_BRED" (Orphaned)"COLOR_END, stdout);

			/* Is the package out-of-date? */
			if (aur[i].outdated > 0) {
				date = pretty_time(aur[i].outdated);
				fprintf(stdout, COLOR_BRED" (Out-of-date: %s)"COLOR_END,
					date);
				free(date);
			}
			fprintf(stdout, "\n ~> %s\n", aur[i].description);
		}

		fputs(COLOR_BLUE":: "COLOR_END, stdout);
		fputs(COLOR_WHITE"Packages to install (eg: 1 2 3):\n", stdout);
		fputs(COLOR_BLUE":: "COLOR_END, stdout);
		/* The last fputs doesn't uses a newline to flush the
		   stdout output. So we need to flush it manually. */
		fflush(stdout);
	} else {
		for (i = 0, j = 1; i < lcount; i++, j++) {
			fprintf(stdout, "%zu aur/", j);
			fprintf(stdout, "%s (%s)", aur[i].name,
				aur[i].version);
			fprintf(stdout, " (+%u %.2lf%%)", aur[i].numvotes,
				aur[i].popularity);

			/* Is there no maintainer? Package must be orphaned. */
			if (aur[i].maintainer == NULL)
			        fputs(" (Orphaned)", stdout);

			/* Is the package out-of-date? */
			if (aur[i].outdated > 0) {
				date = pretty_time(aur[i].outdated);
				fprintf(stdout, " (Out-of-date: %s)", date);
				free(date);
			}
			fprintf(stdout, "\n ~> %s\n", aur[i].description);
		}

		fputs(":: ", stdout);
		fputs("Packages to install (eg: 1 2 3):\n", stdout);
		fputs(":: ", stdout);
		fflush(stdout);
	}

	/* This section is for reading the input stream and parse
	   that stream. After that, download the specific tarball
	   which was provided by the input. */

        /* Fill the buffers with zeros. */
	memset(vstdin, '\0', sizeof(vstdin));
	/* Read input from standard input. */
	read(STDIN_FILENO, vstdin, sizeof(vstdin));

	/* Setup variables. */
	p = vstdin;
	fpkg = 0;
        didx = 1;

	while (*p != '\0') {
		/* If there are space(s), consume and ignore them. */
		if (*p == ' ') {
			p++;
			continue;
		}

		/* Extract the number. */
		inum = safe_atoul(p);
		if (lcount > 1 && inum == 0) {
			/* Keep consuming, even if there are no
			   numbers left. */
			p++;
			continue;
		}
		/* If it's just a the single number (first package),
		   and we didn't looped through, print that we couldn't
		   able to find anything related. */
		if (fpkg == 0) {
			if ((inum - 1) > lcount || aur[inum - 1].url_path == NULL) {
				fputs(" there is nothing to do\n", stderr);
				goto out_cleanup;
			}
		}

		/* lcount is (lcount + 1), so we need to subtract 1.
		   If inum (package input number) is bigger than
		   total number of packages, ignore it and continue
		   the loop. */
		if ((inum - 1) <= (lcount - 1)) {
			/* If URL is missing. The upper branch is to check
			   whether if we overflow than lcount, as it's not
			   possible to index if inum > lcount. */
			/* aur[inum].url_path */
			usz = sizeof(AUR_BASE_URL) + strlen(aur[inum - 1].url_path);
			k = calloc(usz, sizeof(char));
			if (k == NULL)
				err(EXIT_FAILURE, "calloc()");

			base = base_name(aur[inum - 1].url_path);
			if (base == NULL)
				errx(EXIT_FAILURE, "base_name(): Parsed URL is invalid.");

			/* Colors. */
			if (enable_colors)
		        	fprintf(stdout, COLOR_BLUE":: "
					COLOR_PURPLE"(%zu) "
					COLOR_WHITE"Downloading %s...\n"COLOR_END,
					didx, base);
		        else
			        fprintf(stdout,
					":: (%zu) "
					"Downloading %s...\n", didx, base);

			snprintf(k, usz, "%s/"AUR_CGIT_PATH"/%s.tar.gz", AUR_BASE_URL,
				 aur[inum - 1].url_base);
			download_from_url(base, k, 0);

			/* Colors. */
			if (enable_colors)
			        fprintf(stdout,
					COLOR_BLUE":: "COLOR_WHITE
					"~> Extracting %s...\n"COLOR_END, base);
			else
			        fprintf(stdout, ":: ~> Extracting %s...\n", base);

			/* Check whether the file is a gzipped tarball or not. */
			if (likely_targz_magic_sig(base) == 0)
				errx(EXIT_FAILURE,
				     "error: Downloaded archive is not "
				     "a gzipped tarball.");

			/* Decompress the gzipped tarball. */
			targz_decompress_archive(base);

			/* Use the url basename, as it'd be the name of
			   the directory after the extraction. */
		        makepkg_and_install(aur[inum - 1].url_base);
			free(k);

			/* How many packages we've downloaded? */
			didx++;
		}

		/* Break if there's a tab character. They're not handy
		   while working with this. It will parse the first character
		   and will only break if there's any tab character after this. */
		if (*p == '\t')
			goto out_cleanup;

		/* Consume the next character(s) (number) until we
		   reach a space character. Then we can just pass
		   it to over safe_atoi() which will trim these
		   additional spaces automatically. */
		while (*p != ' ')
			p++;

		/* We got our first package. */ 
		fpkg = 1;
	}

	/* If not a single package is there. For example,
	   when you input characters that are not numbers,
	   this will trigger. */
	if (fpkg == 0)
		fputs(" there is nothing to do\n", stderr);

out_cleanup:
	json_value_free(jsch);
	free(jobjs);
	free(aur);
}

/* Request for AUR package information. */
static char *request_aur_info_endpoint(const char *url)
{
        CURL *curl;
	struct curl_memory cm;
	int ret;

	curl = curl_easy_init();
	if (curl == NULL)
		err(EXIT_FAILURE, "curl_easy_init()");

	memset(&cm, '\0', sizeof(struct curl_memory));
        curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, (long)1);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&cm);

	ret = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	curl_global_cleanup();

	if (ret != CURLE_OK)
		err(EXIT_FAILURE, "curl_easy_perform()");

	/* Return the response.  */
	return (cm.resp);
}

/* Format the AUR_INFO_URL. */
static char *format_info_package(const char *pkg)
{
        char *p;
	size_t asz;

	asz = strlen(pkg);
	p = calloc((size_t)9 + asz + sizeof(AUR_INFO_URL), sizeof(char));
	if (p == NULL)
		err(EXIT_FAILURE, "calloc()");

	/* Add proper size and name. */
	memcpy(p, AUR_INFO_URL, sizeof(AUR_INFO_URL) - 1);
        memcpy(p + sizeof(AUR_INFO_URL) - 1, "?arg[]=", (size_t)7);
	memcpy(p + sizeof(AUR_INFO_URL) - 1 + (size_t)7, pkg, asz);

        return (p);
}

/* Format the output and then print it. */
static void format_print_package_info(struct aur_pkg_info aur_info,
				      int enable_colors)
{
	char *pfirst_sub, *plast_mod, *outdated;

	pfirst_sub = pretty_time(aur_info.first_sub);
	plast_mod = pretty_time(aur_info.last_mod);
	if (aur_info.outdated > (time_t)0) {
		outdated = pretty_time(aur_info.outdated);
	} else {
		outdated = strdup("No");
		if (outdated == NULL)
			err(EXIT_FAILURE, "strdup()");
	}

	/* If we've colors enabled. */
	if (enable_colors) {
		fprintf(stdout,
			COLOR_BLUE":: "COLOR_WHITE"Package Name:"COLOR_END" %s\n"
			COLOR_BLUE":: "COLOR_WHITE"Description:"COLOR_END" %s\n"
			COLOR_BLUE":: "COLOR_WHITE"URL:"COLOR_END" %s\n"
			COLOR_BLUE":: "COLOR_WHITE"Version:"COLOR_END" %s\n"
			COLOR_BLUE":: "COLOR_WHITE"Outdated:"COLOR_END" %s\n"
			COLOR_BLUE":: "COLOR_WHITE"Votes:"COLOR_END" %u\n"
			COLOR_BLUE":: "COLOR_WHITE"First Submitted:"COLOR_END" %s\n"
			COLOR_BLUE":: "COLOR_WHITE"Last Modified:"COLOR_END" %s\n"
			COLOR_BLUE":: "COLOR_WHITE"Popularity:"COLOR_END" %.2lf%%\n",
			aur_info.name, aur_info.description, aur_info.url,
			aur_info.version, outdated, aur_info.num_votes,
			pfirst_sub, plast_mod, aur_info.popularity);

	        fprintf(stdout,
			COLOR_BLUE":: "COLOR_WHITE"Depends:"COLOR_END" %s\n",
			aur_info.depends);
		fprintf(stdout,
			COLOR_BLUE":: "COLOR_WHITE"Licenses:"COLOR_END" %s\n",
			aur_info.licenses);
	        fprintf(stdout,
			COLOR_BLUE":: "COLOR_WHITE"Keywords:"COLOR_END" %s\n",
			aur_info.keywords);
		fprintf(stdout,
			COLOR_BLUE":: "COLOR_WHITE"Opt-Depends:"COLOR_END" %s\n",
			aur_info.optdeps);
	} else {
		fprintf(stdout,
			":: Package Name: %s\n"
			":: Description: %s\n"
			":: URL: %s\n"
			":: Version: %s\n"
			":: Outdated: %s\n"
			":: Votes: %u\n"
			":: First Submitted: %s\n"
			":: Last Modified: %s\n"
			":: Popularity: %.2lf%%\n",
			aur_info.name, aur_info.description, aur_info.url,
			aur_info.version, outdated, aur_info.num_votes,
			pfirst_sub, plast_mod, aur_info.popularity);

	        fprintf(stdout, ":: Depends: %s\n", aur_info.depends);
	        fprintf(stdout, ":: Licenses: %s\n", aur_info.licenses);
	        fprintf(stdout, ":: Keywords: %s\n", aur_info.keywords);
	        fprintf(stdout, ":: Opt-Depends: %s\n", aur_info.optdeps);
	}

	/* Unmap all mapped spaces. */
	free(pfirst_sub);
	free(plast_mod);
	free(outdated);
	free(aur_info.depends);
        free(aur_info.licenses);
	free(aur_info.keywords);
	free(aur_info.optdeps);        
}

/* Set all values to aur_pkg_info structure, and then call
   the format function to print it out. */
static void print_package_info(const char *pkg, int enable_colors)
{
        char *fmt, *r, *json;
	const char *vs;
        JSON_Object *jso, *jao;
	JSON_Array *jar;
	size_t asz, bsz, csz, i;
	struct aur_pkg_info aur_info;

	/* Format the URL. */
        fmt = format_info_package(pkg);
	json = request_aur_info_endpoint(fmt);
	jso = json_object(json_parse_string(json));
	jar = json_object_get_array(jso, "results");
	/* Get the first array from the results. */
	jao = json_array_get_object(jar, 0);

	/* Check if results array has some objects. */
	if (json_array_get_count(jar) == 0) {
		free(fmt);
		free(json);
		fprintf(stderr,
			"error: no package was found called '%s'.\n",
			pkg);
		exit(EXIT_FAILURE);
	}

	/* Retrieve information from the JSON. */
	aur_info.name = json_object_get_string(jao, "Name");
        aur_info.description = json_object_get_string(jao, "Description");
	aur_info.url = json_object_get_string(jao, "URL");
	if (aur_info.url == NULL)
		aur_info.url = "none";

	aur_info.version = json_object_get_string(jao, "Version");
	aur_info.outdated = (time_t)json_object_get_number(jao, "OutOfDate");
        aur_info.num_votes = (uint32_t)json_object_get_number(jao, "NumVotes");
	aur_info.first_sub = (time_t)json_object_get_number(jao, "FirstSubmitted");
	aur_info.last_mod = (time_t)json_object_get_number(jao, "LastModified");
	aur_info.popularity = json_object_get_number(jao, "Popularity");

	/* List of depends. */
	asz = json_array_get_count(json_object_get_array(jao, "Depends"));
	aur_info.depends = calloc((size_t)5, sizeof(char));
	if (aur_info.depends == NULL)
	        err(EXIT_FAILURE, "calloc()");

	/* Check if there are depends or not. */
	if (asz >= 1) {
		for (i = 0; i < asz; i++) {
			vs = json_array_get_string(json_object_get_array(jao, "Depends"), i);
			bsz = strlen(aur_info.depends);
			csz = strlen(vs) + (size_t)4;
			r = realloc(aur_info.depends, bsz + csz);
			if (r == NULL)
				err(EXIT_FAILURE, "realloc()");

			aur_info.depends = r;
			memcpy(aur_info.depends + bsz, vs, csz - 4);
			memcpy(aur_info.depends + bsz + csz - 4, " ", 2);
		}
	} else {
		memcpy(aur_info.depends, "none", (size_t)4);
	}

	/* List of licenses. */
	asz = json_array_get_count(json_object_get_array(jao, "License"));
	aur_info.licenses = calloc((size_t)5, sizeof(char));
	if (aur_info.licenses == NULL)
		err(EXIT_FAILURE, "calloc()");

	/* Check if there are licenses or not. */
	if (asz >= 1) {
		for (i = 0; i < asz; i++) {
			vs = json_array_get_string(json_object_get_array(jao, "License"), i);
			bsz = strlen(aur_info.licenses);
			csz = strlen(vs) + (size_t)4;
			r = realloc(aur_info.licenses, bsz + csz);
			if (r == NULL)
				err(EXIT_FAILURE, "realloc()");

			aur_info.licenses = r;
			memcpy(aur_info.licenses + bsz, vs, csz - 4);
			memcpy(aur_info.licenses + bsz + csz - 4, " ", 2);
		}
	} else {
	        memcpy(aur_info.licenses, "none", (size_t)4);
	}

	/* List of keywords. */
	asz = json_array_get_count(json_object_get_array(jao, "Keywords"));
	aur_info.keywords = calloc((size_t)5, sizeof(char));
	if (aur_info.keywords == NULL)
		err(EXIT_FAILURE, "calloc()");

	/* Check if there are keywords or not. */
	if (asz >= 1) {
		for (i = 0; i < asz; i++) {
			vs = json_array_get_string(json_object_get_array(jao, "Keywords"), i);
			bsz = strlen(aur_info.keywords);
			csz = strlen(vs) + (size_t)4;
			r = realloc(aur_info.keywords, bsz + csz);
			if (r == NULL)
				err(EXIT_FAILURE, "realloc()");

			aur_info.keywords = r;
			memcpy(aur_info.keywords + bsz, vs, csz - 4);
			memcpy(aur_info.keywords + bsz + csz - 4, " ", 2);
		}
	} else {
	        memcpy(aur_info.keywords, "none", (size_t)4);
	}

	/* List of optional depends. */
	asz = json_array_get_count(json_object_get_array(jao, "OptDepends"));
	aur_info.optdeps = calloc((size_t)5, sizeof(char));
	if (aur_info.optdeps == NULL)
	        err(EXIT_FAILURE, "calloc()");

	/* Check if there are optional depends or not. */
	if (asz >= 1) {
		for (i = 0; i < asz; i++) {
			vs = json_array_get_string(json_object_get_array(jao, "OptDepends"), i);
			bsz = strlen(aur_info.optdeps);
			csz = strlen(vs) + (size_t)4;
			r = realloc(aur_info.optdeps, bsz + csz);
			if (r == NULL)
				err(EXIT_FAILURE, "realloc()");

			aur_info.optdeps = r;
			memcpy(aur_info.optdeps + bsz, vs, csz - 4);
			memcpy(aur_info.optdeps + bsz + csz - 4, " ", 2);
		}
	} else {
		memcpy(aur_info.optdeps, "none", (size_t)4);
        }

	format_print_package_info(aur_info, enable_colors);
	free(fmt);
	free(json);
}

/* Print usage. */
static void print_usage(int status, int enable_colors)
{
	FILE *out;

	out = status == EXIT_SUCCESS
		? stdout : stderr;

	if (enable_colors) {
		fputs("aurpkg - A small and lightweight AUR helper\n"
		      UNDERLINE COLOR_WHITE"Usage:"COLOR_END
		      COLOR_WHITE" aurpkg"COLOR_END" [OPTIONS]..\n\n"
		      UNDERLINE COLOR_WHITE"Options:\n"COLOR_END
		      COLOR_WHITE"  -s, --search"COLOR_END
		      "\tSearch for a package in the AUR repository\n"
		      COLOR_WHITE"  -i, --info"COLOR_END
		      "\tRetrieve information about a package\n"
		      COLOR_WHITE"  -g, --get"COLOR_END
		      "\tDownload anything from a specified URL\n"
		      COLOR_WHITE"  -h, --help"COLOR_END
		      "\tDisplay this help message\n", out);
		fputs(UNDERLINE COLOR_WHITE"\nOptional:\n"COLOR_END
		      COLOR_WHITE"  -c, --colors"COLOR_END
		      "\tEnable colored output\n", out);
	} else {
		fputs("aurpkg - A small and lightweight AUR helper\n"
		      "Usage: aurpkg [OPTIONS]..\n\n"
		      "Options:\n"
		      "  -s, --search\tSearch for a package in the AUR repository\n"
		      "  -i, --info\tRetrieve information about a package\n"
		      "  -g, --get\tDownload anything from a specified URL\n"
		      "  -h, --help\tDisplay this help message\n", out);
		fputs("\nOptional:\n"
		     "  -c, --colors\tEnable colored output\n", out);
	}
	/* TODO: add usage here. Cleanup, test arguments, add readme. */
	exit(status);
}

/* The main function. */
int main(int argc, char **argv)
{
	char *json;
	int i;
	struct arg_opts opts = {0};
	struct option lopts[] = {
		{ "search",  required_argument, NULL, 's' },
		{ "info",    required_argument, NULL, 'i' },
		{ "colors",  no_argument,       NULL, 'c' },
		{ "help",    no_argument,       NULL, 'h' },
		{ NULL,      0,                 NULL,  0  },
	};

	if (argc < 2)
		print_usage(EXIT_FAILURE, 0);

        for (;;) {
		opts.c = getopt_long(argc, argv, "s:i:ch", lopts, NULL);
		if (opts.c == -1)
			break;

	        switch (opts.c) {
		case 's':
			/* Option: "-s'. */
			opts.is_search = 1;
			break;
		case 'i':
			/* Option: "-i'. */
			opts.is_info = 1;
			break;
		case 'c':
			/* Option: "-c'. */
			opts.is_colors = 1;
			break;
		case 'h':
			/* Option: "-h'. */
			opts.is_help = 1;
			break;
		default:
			/* Anything else as option, just ignore them. */
			break;
		}
	}

	/* If option is "-s" or "--search". */
	if (opts.is_search) {
		/* If option is "-sc", enable color as well. */
		if (argv[optind - 1][0] == '-' &&
		    argv[optind - 1][1] == 's' &&
		    argv[optind - 1][2] == 'c') {
			opts.is_colors = 1;
			optind++;
		}
	
		json = search_for_pkg(argv[optind - 1]);
		print_search_results(json, opts.is_colors);
		free(json);
        }

	/* If option is "-i", "--info". */
	if (opts.is_info) {
		/* If option is "-ic", enable color as well. */
	        if (argv[optind - 1][0] == '-' &&
		    argv[optind - 1][1] == 'i' &&
		    argv[optind - 1][2] == 'c') {
			opts.is_colors = 1;
			optind++;
		}

		/* Iterate over. */
		for (i = optind - 1; i < argc; i++) {
			print_package_info(argv[i], opts.is_colors);
			if (i != (argc - 1)) {
				if (opts.is_colors)
					fputs(COLOR_LGREEN
					      "********************************\n"
					      COLOR_END, stdout);
				else
					fputs("********************************\n",
					      stdout);
			}
		}
	}

	/* If option is "-h", "--help". */
	if (opts.is_help)
	        print_usage(EXIT_SUCCESS, opts.is_colors);
}
