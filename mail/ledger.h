/*
 * The Bills view: your bills as a ledger, a month at a time, newest
 * first, each month with what's due and what's still unpaid. A row's
 * payee, amount, due date and what it's for are read from the bill with AI
 * (bills.h); whether it's paid is yours to tick (or it's paid
 * automatically). Like the Newsletters view, it spans every folder.
 */
#ifndef GEMWM_MAIL_LEDGER_H
#define GEMWM_MAIL_LEDGER_H

#include <gtk/gtk.h>
#include "cache.h"

struct ledger;

/* A bill chosen: open it, from its folder. */
typedef void (*ledger_open_fn)(const char *mailbox, guint32 uid, void *data);

struct ledger *ledger_new(struct cache *cache, ledger_open_fn open, void *data);
GtkWidget *ledger_widget(struct ledger *l);
/* Reads the bills again; the selection's kept. reading: how many are
 * still being read, for the title. */
void ledger_load(struct ledger *l, int reading);
void ledger_focus(struct ledger *l);

#endif
