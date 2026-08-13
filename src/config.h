#ifndef INIS_CONFIG_H
#define INIS_CONFIG_H

#include "inis.h"

#include "bind.h"
#include "rules.h"

struct inis_config_var {
	char name[INIS_MAX_NAME];
	char value[INIS_MAX_ARGS];
};

struct inis_config {
	char terminal[INIS_MAX_NAME];
	char menu[INIS_MAX_NAME];
	char file_manager[INIS_MAX_NAME];
	struct inis_config_var vars[INIS_MAX_CONFIG_VARS];
	size_t var_count;
	char startup_commands[INIS_MAX_STARTUP_COMMANDS][INIS_MAX_ARGS];
	size_t startup_command_count;
	int gaps_in;
	int gaps_out;
	int border_size;
	double master_ratio;
	/* Default reserved space for external bars, per edge, in pixels. */
	int reserved_top;
	int reserved_bottom;
	int reserved_left;
	int reserved_right;
};

void inis_config_init(struct inis_config *config);
int inis_config_load_file(struct inis_config *config,
    struct inis_binding *bindings, size_t *binding_count,
    struct inis_rule *rules, size_t *rule_count,
    const char *path);
int inis_config_apply_line(struct inis_config *config,
    struct inis_binding *bindings, size_t *binding_count,
    struct inis_rule *rules, size_t *rule_count,
    const char *line);

#endif
