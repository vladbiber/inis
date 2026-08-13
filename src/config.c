#include "config.h"

#include "config.def.h"
#include "log.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void config_set_var(struct inis_config *config, const char *name,
    const char *value);

void
inis_config_init(struct inis_config *config)
{
	memset(config, 0, sizeof(*config));
	snprintf(config->terminal, sizeof(config->terminal), "%s", INIS_DEFAULT_TERMINAL);
	snprintf(config->menu, sizeof(config->menu), "%s", INIS_DEFAULT_MENU);
	snprintf(config->file_manager, sizeof(config->file_manager), "%s", INIS_DEFAULT_FILE_MANAGER);
	config_set_var(config, "mainMod", "SUPER");
	config_set_var(config, "terminal", config->terminal);
	config_set_var(config, "menu", config->menu);
	config_set_var(config, "fileManager", config->file_manager);
	config->gaps_in = INIS_DEFAULT_GAPS_IN;
	config->gaps_out = INIS_DEFAULT_GAPS_OUT;
	config->border_size = INIS_DEFAULT_BORDER_SIZE;
	config->master_ratio = INIS_DEFAULT_MASTER_RATIO;
}

static char *
trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	if (*s == '\0')
		return s;

	end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';
	return s;
}

static void
copy_value(char *dst, size_t dst_size, const char *src)
{
	size_t i = 0;

	while (isspace((unsigned char)*src))
		src++;
	if (dst_size == 0)
		return;
	while (src[i] != '\0' && i + 1 < dst_size) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

static int
config_find_var(const struct inis_config *config, const char *name)
{
	size_t i;

	for (i = 0; i < config->var_count; i++) {
		if (strcmp(config->vars[i].name, name) == 0)
			return (int)i;
	}
	return -1;
}

static void
config_set_var(struct inis_config *config, const char *name, const char *value)
{
	int index;

	if (name == NULL || name[0] == '\0' || value == NULL)
		return;

	index = config_find_var(config, name);
	if (index < 0) {
		if (config->var_count >= INIS_MAX_CONFIG_VARS) {
			inis_warn("config variable ignored: limit reached");
			return;
		}
		index = (int)config->var_count++;
		snprintf(config->vars[index].name, sizeof(config->vars[index].name),
		    "%s", name);
	}

	copy_value(config->vars[index].value,
	    sizeof(config->vars[index].value), value);
}

static const char *
config_get_var(const struct inis_config *config, const char *name)
{
	int index = config_find_var(config, name);

	if (index < 0)
		return NULL;
	return config->vars[index].value;
}

static bool
is_var_char(int c)
{
	return isalnum((unsigned char)c) || c == '_';
}

static void
expand_vars(const struct inis_config *config, char *dst, size_t dst_size,
    const char *src)
{
	size_t out = 0;
	size_t i = 0;

	if (dst_size == 0)
		return;

	while (src[i] != '\0' && out + 1 < dst_size) {
		char name[INIS_MAX_NAME];
		const char *value;
		size_t n = 0;
		size_t j;

		if (src[i] != '$') {
			dst[out++] = src[i++];
			continue;
		}

		i++;
		while (src[i] != '\0' && is_var_char((unsigned char)src[i]) &&
		    n + 1 < sizeof(name))
			name[n++] = src[i++];
		name[n] = '\0';

		if (n == 0) {
			dst[out++] = '$';
			continue;
		}

		value = config_get_var(config, name);
		if (value == NULL) {
			if (out + 1 < dst_size)
				dst[out++] = '$';
			for (j = 0; j < n && out + 1 < dst_size; j++)
				dst[out++] = name[j];
			continue;
		}

		for (j = 0; value[j] != '\0' && out + 1 < dst_size; j++)
			dst[out++] = value[j];
	}

	dst[out] = '\0';
}

static int
add_startup_command(struct inis_config *config, const char *command)
{
	if (command == NULL || command[0] == '\0')
		return -1;
	if (config->startup_command_count >= INIS_MAX_STARTUP_COMMANDS)
		return -1;

	copy_value(config->startup_commands[config->startup_command_count],
	    sizeof(config->startup_commands[config->startup_command_count]),
	    command);
	config->startup_command_count++;
	return 0;
}

int
inis_config_apply_line(struct inis_config *config,
    struct inis_binding *bindings, size_t *binding_count,
    struct inis_rule *rules, size_t *rule_count,
    const char *line)
{
	char buf[INIS_MAX_LINE];
	char expanded[INIS_MAX_LINE];
	char key[INIS_MAX_NAME];
	char name[INIS_MAX_NAME];
	char value[INIS_MAX_ARGS];
	char *s;
	int n;

	snprintf(buf, sizeof(buf), "%s", line);
	s = strchr(buf, '#');
	if (s != NULL)
		*s = '\0';
	s = trim(buf);
	if (s[0] == '\0')
		return 0;

	expand_vars(config, expanded, sizeof(expanded), s);
	s = expanded;

	if (strncmp(s, "exec-once ", 10) == 0) {
		s = trim(s + 10);
		if (add_startup_command(config, s) != 0)
			return -1;
		return 0;
	}

	if (strncmp(s, "bind ", 5) == 0 || strncmp(s, "bindm ", 6) == 0) {
		if (*binding_count >= INIS_MAX_BINDINGS)
			return -1;
		if (inis_bind_parse_line(&bindings[*binding_count], s) != 0)
			return -1;
		(*binding_count)++;
		return 0;
	}

	if (strncmp(s, "windowrule ", 11) == 0) {
		if (*rule_count >= INIS_MAX_RULES)
			return -1;
		if (inis_rule_parse_line(&rules[*rule_count], s) != 0)
			return -1;
		(*rule_count)++;
		return 0;
	}

	if (strncmp(s, "reserved ", 9) == 0) {
		int top, bottom, left, right;

		if (sscanf(s + 9, "%d %d %d %d", &top, &bottom, &left, &right) != 4)
			return -1;
		config->reserved_top = top >= 0 ? top : 0;
		config->reserved_bottom = bottom >= 0 ? bottom : 0;
		config->reserved_left = left >= 0 ? left : 0;
		config->reserved_right = right >= 0 ? right : 0;
		return 0;
	}

	n = sscanf(s, "%63s %63s %255[^\n]", key, name, value);
	if (n < 2)
		return -1;

	if (strcmp(key, "set") == 0) {
		if (n < 3)
			return -1;
		config_set_var(config, name, value);
		if (strcmp(name, "terminal") == 0)
			copy_value(config->terminal, sizeof(config->terminal), value);
		else if (strcmp(name, "menu") == 0)
			copy_value(config->menu, sizeof(config->menu), value);
		else if (strcmp(name, "fileManager") == 0)
			copy_value(config->file_manager, sizeof(config->file_manager), value);
		return 0;
	}

	if (strcmp(key, "general") == 0) {
		if (n < 3)
			return -1;
		if (strcmp(name, "gaps_in") == 0) {
			int v = atoi(value);
			config->gaps_in = v >= 0 ? v : 0;
		} else if (strcmp(name, "gaps_out") == 0) {
			int v = atoi(value);
			config->gaps_out = v >= 0 ? v : 0;
		} else if (strcmp(name, "border_size") == 0) {
			int v = atoi(value);
			config->border_size = v >= 0 ? v : 0;
		} else if (strcmp(name, "master_ratio") == 0) {
			double v = atof(value);
			if (v > 0.1 && v < 0.9)
				config->master_ratio = v;
		} else
			inis_debug("ignored general option: %s", name);
		return 0;
	}

	if (strcmp(key, "monitor") == 0 ||
	    strcmp(key, "decoration") == 0 ||
	    strcmp(key, "animations") == 0) {
		inis_debug("ignored planned config key: %s", key);
		return 0;
	}

	inis_warn("unknown config line: %s", s);
	return -1;
}

int
inis_config_load_file(struct inis_config *config,
    struct inis_binding *bindings, size_t *binding_count,
    struct inis_rule *rules, size_t *rule_count,
    const char *path)
{
	FILE *fp;
	char line[INIS_MAX_LINE];
	unsigned int lineno = 0;
	int errors = 0;

	fp = fopen(path, "r");
	if (fp == NULL)
		return -1;

	while (fgets(line, sizeof(line), fp) != NULL) {
		lineno++;
		if (inis_config_apply_line(config, bindings, binding_count,
		    rules, rule_count, line) != 0) {
			inis_warn("%s:%u: invalid config line", path, lineno);
			errors++;
		}
	}

	fclose(fp);
	return errors == 0 ? 0 : -1;
}
