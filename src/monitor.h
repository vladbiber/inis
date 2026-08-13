#ifndef INIS_MONITOR_H
#define INIS_MONITOR_H

#include "inis.h"
#include "damage.h"

struct swc_screen;

struct inis_monitor {
	char name[INIS_MAX_NAME];
	struct inis_rect geometry;
	/* usable: effective layout area (backend usable inset by reserved). */
	struct inis_rect usable;
	/* backend_usable: raw usable area reported by the backend. */
	struct inis_rect backend_usable;
	/* reserved: space claimed for external bars, per edge, in pixels. */
	struct {
		int top;
		int bottom;
		int left;
		int right;
	} reserved;
	int scale;
	int refresh_mhz;
	unsigned int active_workspace;
	unsigned int previous_workspace;
	bool enabled;
	struct inis_damage damage;
	struct swc_screen *swc;
};

void inis_monitor_init(struct inis_monitor *monitor, const char *name);

#endif
