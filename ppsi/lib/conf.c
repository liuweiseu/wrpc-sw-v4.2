/*
 * Copyright (C) 2013 CERN (www.cern.ch)
 * Authors: Aurelio Colosimo, Alessandro Rubini
 *
 * Released according to the GNU LGPL, version 2.1 or any later version.
 */

#include <ppsi/ppsi.h>
/* This file is built in hosted environments, so following headers are Ok */
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

static inline struct pp_instance *CUR_PPI(struct pp_globals *ppg)
{
	if (ppg->cfg.cur_ppi_n < 0)
		return NULL;
	return INST(ppg, ppg->cfg.cur_ppi_n);
}

/* A "port" (or "link", for compatibility) line creates or uses a pp instance */
static int f_port(struct pp_argline *l, int lineno, struct pp_globals *ppg,
		  union pp_cfg_arg *arg)
{
	int i;

	/* First look for an existing port with the same name */
	for ( i = 0; i < ppg->nlinks; i++) {
		ppg->cfg.cur_ppi_n = i;
		if (!strcmp(arg->s, CUR_PPI(ppg)->cfg.port_name))
			return 0;
	}
	/* check if there are still some free pp_instances to be used */
	if (ppg->nlinks >= ppg->max_links) {
		pp_printf("config line %i: out of available ports\n",
			  lineno);
		/* we are out of available ports. set cur_ppi_n to -1 so if
		 * someone tries to set some parameter to this pp_instance it
		 * will cause an error, instead of overwrite the parameters of
		 * antother pp_instance */
		ppg->cfg.cur_ppi_n = -1;
		return -1;
	}
	ppg->cfg.cur_ppi_n = ppg->nlinks;

	 /* FIXME: strncpy (it is missing in bare archs by now) */
	strcpy(CUR_PPI(ppg)->cfg.port_name, arg->s);
	strcpy(CUR_PPI(ppg)->cfg.iface_name, arg->s); /* default iface name */
	ppg->nlinks++;
	return 0;
}

#define CHECK_PPI(need) /* Quick hack to factorize errors later */ 	\
	({if (need && !CUR_PPI(ppg)) {		\
		pp_printf("config line %i: no port for this config\n", lineno);\
		return -1; \
	} \
	if (!need && CUR_PPI(ppg)) { \
		pp_printf("config line %i: global config under \"port\"\n", \
			  lineno); \
		return -1; \
	}})

static inline void ASSIGN_INT_FIELD(struct pp_argline *l,
				    struct pp_globals *ppg,
				    int v)
{
	if (l->needs_port)
		*(int *)(((void *)CUR_PPI(ppg)) + l->field_offset) = v;
	else
		*(int *)(((void *)GOPTS(ppg)) + l->field_offset) = v;
}

int f_simple_int(struct pp_argline *l, int lineno,
		 struct pp_globals *ppg, union pp_cfg_arg *arg)
{
	CHECK_PPI(l->needs_port);
	ASSIGN_INT_FIELD(l, ppg, arg->i);
	return 0;
}

static int f_if(struct pp_argline *l, int lineno, struct pp_globals *ppg,
		union pp_cfg_arg *arg)
{
	CHECK_PPI(1);
	strcpy(CUR_PPI(ppg)->cfg.iface_name, arg->s);
	return 0;
}

/* Diagnostics can be per-port or global */
static int f_diag(struct pp_argline *l, int lineno, struct pp_globals *ppg,
		  union pp_cfg_arg *arg)
{
	unsigned long level = pp_diag_parse(arg->s);

	if (ppg->cfg.cur_ppi_n >= 0)
		CUR_PPI(ppg)->d_flags = level;
	else
		pp_global_d_flags = level;
	return 0;
}

/* VLAN support is per-port, and it depends on configuration itmes */
static int f_vlan(struct pp_argline *l, int lineno, struct pp_globals *ppg,
		  union pp_cfg_arg *arg)
{
	struct pp_instance *ppi = CUR_PPI(ppg);
	int i, n, *v;
	char ch, *s;

	CHECK_PPI(1);

	/* Refuse to add vlan support in non-raw mode */
	if (ppi->proto == PPSI_PROTO_UDP) {
		pp_printf("config line %i: VLANs with UDP: not supported\n",
			  lineno);
		return -1;
	}
	/* If there is no support, just warn */
	if (CONFIG_VLAN_ARRAY_SIZE == 0) {
		pp_printf("Warning: config line %i ignored:"
			  " this PPSI binary has no VLAN support\n",
			  lineno);
		return 0;
	}
	if (ppi->nvlans)
		pp_printf("Warning: config line %i overrides "
			  "previous vlan settings\n", lineno);

	s = arg->s;
	for (v = ppi->vlans, n = 0; n < CONFIG_VLAN_ARRAY_SIZE; n++, v++) {
		i = sscanf(s, "%i %c", v, &ch);
		if (!i)
			break;
		if (*v > 4095 || *v < 0) {
			pp_printf("config line %i: vlan out of range: %i "
				  "(valid is 0..4095)\n", lineno, *v);
			return -1;
		}
		if (i == 2 && ch != ',') {
			pp_printf("config line %i: unexpected char '%c' "
				  "after %i\n", lineno, ch, *v);
			return -1;
		}
		if (i == 2)
			s = strchr(s, ',') + 1;
		else break;
	}
	if (n == CONFIG_VLAN_ARRAY_SIZE) {
		pp_printf("config line %i: too many vlans (%i): max is %i\n",
			  lineno, n + 1, CONFIG_VLAN_ARRAY_SIZE);
		return -1;
	}
	ppi->nvlans = n + 1; /* item "n" has been assigend too, 0-based */

	for (i = 0; i < ppi->nvlans; i++)
		pp_diag(NULL, config, 2, "  parsed vlan %4i for %s (%s)\n",
			ppi->vlans[i], ppi->cfg.port_name, ppi->cfg.iface_name);
	pp_diag(NULL, config, 2, "role %i\n", ppi->role);
	if (ppi->role != PPSI_ROLE_MASTER && ppi->nvlans > 1) {
		pp_printf("config line %i: too many vlans (%i) for slave "
			  "or auto role\n", lineno, ppi->nvlans);
		return -1;
	}
	ppi->proto = PPSI_PROTO_VLAN;
	return 0;
}

static int f_servo_pi(struct pp_argline *l, int lineno,
		      struct pp_globals *ppg, union pp_cfg_arg *arg)
{
	int n1, n2;

	CHECK_PPI(0);
	n1 = arg->i2[0]; n2 = arg->i2[1];
	/* no negative or zero attenuation */
	if (n1 < 1 || n2 < 1)
		return -1;
	GOPTS(ppg)->ap = n1;
	GOPTS(ppg)->ai = n2;
	return 0;
}

static int f_announce_intvl(struct pp_argline *l, int lineno,
			    struct pp_globals *ppg, union pp_cfg_arg *arg)
{
	int i = arg->i;

	CHECK_PPI(0);
	if (i < 0 || i > 4) {
		i = i < 0 ? 0 : 4;
		pp_printf("config line %i: announce interval out of range: %i, "
			  "forced to %i\n", lineno, arg->i, i);
	}
	GOPTS(ppg)->announce_intvl = i;
	return 0;
}

/* These are the tables for the parser */
static struct pp_argname arg_proto[] = {
	{"raw", PPSI_PROTO_RAW},
	{"udp", PPSI_PROTO_UDP},
	/* PROTO_VLAN is an internal modification of PROTO_RAW */
	{},
};
static struct pp_argname arg_role[] = {
	{"auto", PPSI_ROLE_AUTO},
	{"master",PPSI_ROLE_MASTER},
	{"slave", PPSI_ROLE_SLAVE},
	{},
};
static struct pp_argname arg_ext[] = {
	{"none", PPSI_EXT_NONE},
	{"whiterabbit", PPSI_EXT_WR},
	{},
};
static struct pp_argname arg_mech[] = {
	{"request-response", PP_E2E_MECH},
	{"delay", PP_E2E_MECH},
	{"e2e", PP_E2E_MECH},
#if CONFIG_HAS_P2P == 1
	{"peer-delay", PP_P2P_MECH},
	{"pdelay", PP_P2P_MECH},
	{"p2p", PP_P2P_MECH},
#endif
	{},
};

static struct pp_argline pp_global_arglines[] = {
	LEGACY_OPTION(f_port, "port", ARG_STR),
	LEGACY_OPTION(f_port, "link", ARG_STR), /* Old name for port */
	LEGACY_OPTION(f_if, "iface", ARG_STR),
	INST_OPTION_INT("proto", ARG_NAMES, arg_proto, proto),
	INST_OPTION_INT("role", ARG_NAMES, arg_role, role),
	INST_OPTION_INT("extension", ARG_NAMES, arg_ext, cfg.ext),
	INST_OPTION_INT("mechanism", ARG_NAMES, arg_mech, cfg.mech),
	LEGACY_OPTION(f_vlan, "vlan", ARG_STR),
	LEGACY_OPTION(f_diag, "diagnostic", ARG_STR),
	RT_OPTION_INT("clock-class", ARG_INT, NULL, clock_quality.clockClass),
	RT_OPTION_INT("clock-accuracy", ARG_INT, NULL,
		      clock_quality.clockAccuracy),
	RT_OPTION_INT("clock-allan-variance", ARG_INT, NULL,
		      clock_quality.offsetScaledLogVariance),
	LEGACY_OPTION(f_servo_pi, "servo-pi", ARG_INT2),
	RT_OPTION_INT("domain-number", ARG_INT, NULL, domain_number),
	LEGACY_OPTION(f_announce_intvl, "announce-interval", ARG_INT),
	RT_OPTION_INT("sync-interval", ARG_INT, NULL, sync_intvl),
	RT_OPTION_INT("priority1", ARG_INT, NULL, prio1),
	RT_OPTION_INT("priority2", ARG_INT, NULL, prio2),
	{}
};

/* Provide default empty argument lines for architecture and extension */
struct pp_argline pp_arch_arglines[] __attribute__((weak)) = {
	{}
};
struct pp_argline pp_ext_arglines[] __attribute__((weak)) = {
	{}
};

/* local implementation of isblank() and isdigit() for bare-metal users */
static int blank(int c)
{
	return c == ' '  || c == '\t' || c == '\n';
}

static int digit(char c)
{
	return (('0' <= c) && (c <= '9'));
}

static char *first_word(char *line, char **rest)
{
	char *ret;
	int l = strlen(line) -1;

	/* remove trailing blanks */
	while (l >= 0 && blank(line[l]))
		line [l--] = '\0';

	/* skip leading blanks to find first word */
	while (*line && blank(*line))
		line++;
	ret = line;
	/* find next blank and thim there*/
	while (*line && !blank(*line))
		line++;
	if (*line) {
		*line = '\0';
		line++;
	}
	*rest = line;
	return ret;
}

static int parse_time(struct pp_cfg_time *ts, char *s)
{
	long sign = 1;
	long num;
	int i;

	/* skip leading blanks */
	while (*s && blank(*s))
		s++;
	/* detect sign */
	if (*s == '-') {
		sign = -1;
		s++;
	} else if (*s == '+') {
		s++;
	}

	if (!*s)
		return -1;

	/* parse integer part */
	num = 0;
	while (*s && digit(*s)) {
		num *= 10;
		num += *s - '0';
		s++;
	}

	ts->tv_sec = sign * num;
	ts->tv_nsec = 0;

	if (*s == '\0')
		return 0; // no decimals
	// else
	if (*s != '.')
		return -1;

	/* parse decimals */
	s++; // skip '.'
	num = 0;
	for (i = 0; (i < 9) && *s && digit(*s); i++) {
		num *= 10;
		num += *s - '0';;
		s++;
	}
	if (*s) // more than 9 digits or *s is not a digit
		return -1;
	for (; i < 9; i++) // finish to scale nanoseconds
		num *=10;
	ts->tv_nsec = sign * num;
	return 0;
}

static int pp_config_line(struct pp_globals *ppg, char *line, int lineno)
{
	union pp_cfg_arg cfg_arg;
	struct pp_argline *l;
	struct pp_argname *n;
	char *word;

	pp_diag(NULL, config, 2, "parsing line %i: \"%s\"\n", lineno, line);
	word = first_word(line, &line);
	/* now line points to the next word, with no leading blanks */

	if (word[0] == '#')
		return 1;
	if (!*word) {
		/* empty or blank-only
		 * FIXME: this sets cur_ppi_n to an unvalid value. this means
		 * that every blank line in config file unsets the current
		 * pp_instance being configured. For this reason after a blank
		 * line in config file the pp_instance to be configured needs to
		 * be stated anew. Probably this is a desired feature because
		 * this behavior was already present here with the previous
		 * implementation, based on pointers. Is this sane? This means
		 * configuration file is somehow indentation-dependent (because
		 * presence or absence of blank lines matters). This feature is
		 * not documented anywhere AFAIK. If it's a feature it should
		 * be. If its a bug it should be fixed.
		 */
		ppg->cfg.cur_ppi_n = -1;
		return 0;
	}

	/* Look for the configuration keyword in global, arch, ext */
	for (l = pp_global_arglines; l->f; l++)
		if (!strcmp(word, l->keyword))
			break;
	if (!l->f)
		for (l = pp_arch_arglines; l->f; l++)
			if (!strcmp(word, l->keyword))
				break;
	if (!l->f)
		for (l = pp_ext_arglines; l->f; l++)
			if (!strcmp(word, l->keyword))
				break;

	if (!l->f) {
		pp_error("line %i: no such keyword \"%s\"\n", lineno, word);
		return -1;
	}

	if ((l->t != ARG_NONE) && (!*line)) {
		pp_error("line %i: no argument for option \"%s\"\n", lineno,
									word);
		return -1;
	}

	switch(l->t) {

	case ARG_NONE:
		break;

	case ARG_INT:
		if (sscanf(line, "%i", &(cfg_arg.i)) != 1) {
			pp_error("line %i: \"%s\": not int\n", lineno, word);
			return -1;
		}
		break;

	case ARG_INT2:
		if (sscanf(line, "%i,%i", cfg_arg.i2, &cfg_arg.i2[1]) < 0) {
			pp_error("line %i: wrong arg \"%s\" for \"%s\"\n",
				 lineno, line, word);
			return -1;
		}
		break;

	case ARG_STR:
		while (*line && blank(*line))
			line++;

		cfg_arg.s = line;
		break;

	case ARG_NAMES:
		for (n = l->args; n->name; n++)
			if (!strcmp(line, n->name))
				break;
		if (!n->name) {
			pp_error("line %i: wrong arg \"%s\" for \"%s\"\n",
				 lineno, line, word);
			return -1;
		}
		cfg_arg.i = n->value;
		break;

	case ARG_TIME:
		if(parse_time(&cfg_arg.ts, line)) {
			pp_error("line %i: wrong arg \"%s\" for \"%s\"\n",
				 lineno, line, word);
			return -1;
		}
		break;
	}

	if (l->f(l, lineno, ppg, &cfg_arg))
		return -1;

	return 0;
}

/* Parse a whole string by splitting lines at '\n' or ';' */
static int pp_parse_conf(struct pp_globals *ppg, char *conf, int len)
{
	int ret, in_comment = 0, errcount = 0;
	char *line, *rest, term;
	int lineno = 1;

	line = conf;
	/* clear current ppi, don't store current ppi across different
	 * configuration files or strings */
	ppg->cfg.cur_ppi_n = -1;
	/* parse config */
	do {
		for (rest = line;
		     *rest && *rest != '\n' && *rest != ';'; rest++)
			;
		term = *rest;
		*rest = '\0';
		ret = 0;
		if (!in_comment) {
			if (*line)
				ret = pp_config_line(ppg, line, lineno);
			if (ret == 1)
				in_comment = 1;
			if (ret < 0)
				errcount++;
		}

		line = rest + 1;
		if (term == '\n') {
			lineno++;
			in_comment = 0;
		}
	} while (term); /* if terminator was already 0, we are done */
	return errcount ? -1 : 0;
}

/* Open a file, warn if not found */
static int pp_open_conf_file(char *name)
{
	int fd;

	if (!name)
		return -1;
	fd = open(name, O_RDONLY);
	if (fd >= 0) {
		pp_printf("Using config file %s\n", name);
		return fd;
	}
	pp_printf("Warning: %s: %s\n", name, strerror(errno));
	return -1;
}

/*
 * This is one of the public entry points, opening a file
 *
 * "force" means that the called wants to open *this file.
 * In this case we count it even if it fails, so the default
 * config file is not used
 */
int pp_config_file(struct pp_globals *ppg, int force, char *fname)
{
	int conf_fd, i, conf_len = 0;
	int r = 0, next_r;
	struct stat conf_fs;
	char *conf_buf;

	conf_fd = pp_open_conf_file(fname);
	if (conf_fd < 0) {
		if (force)
			ppg->cfg.cfg_items++;
		return -1;
	}
	ppg->cfg.cfg_items++;

	/* read the whole file, it is split up later on */

	fstat(conf_fd, &conf_fs);
	conf_buf = calloc(1, conf_fs.st_size + 2);

	do {
		next_r = conf_fs.st_size - conf_len;
		r = read(conf_fd, &conf_buf[conf_len], next_r);
		if (r <= 0)
			break;
		conf_len = strlen(conf_buf);
	} while (conf_len < conf_fs.st_size);
	close(conf_fd);

	i = pp_parse_conf(ppg, conf_buf, conf_len);
	free(conf_buf);
	return i;
}

/*
 * This is the other public entry points, opening a file
 */
int pp_config_string(struct pp_globals *ppg, char *s)
{
	return pp_parse_conf(ppg, s, strlen(s));
}

