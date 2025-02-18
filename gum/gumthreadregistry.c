/*
 * Copyright (C) 2025 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumthreadregistry.h"

#include "gum-init.h"
#include "gumcloak.h"
#include "gumthreadregistry-priv.h"

#define GUM_THREAD_REGISTRY_LOCK(r) g_rec_mutex_lock (&(r)->mutex)
#define GUM_THREAD_REGISTRY_UNLOCK(r) g_rec_mutex_unlock (&(r)->mutex)

typedef enum {
  GUM_THREAD_REGISTRY_CREATED,
  GUM_THREAD_REGISTRY_ACTIVATED,
} GumThreadRegistryState;

struct _GumThreadRegistry
{
  GObject parent;

  GRecMutex mutex;
  GumThreadRegistryState state;
  GPtrArray * threads;
};

enum
{
  THREAD_ADDED,
  THREAD_RENAMED,
  THREAD_REMOVED,
  LAST_SIGNAL
};

static void gum_thread_registry_dispose (GObject * object);
static void gum_thread_registry_finalize (GObject * object);

static void gum_deinit_thread_registry (void);

G_DEFINE_TYPE (GumThreadRegistry, gum_thread_registry, G_TYPE_OBJECT)

static guint gum_thread_registry_signals[LAST_SIGNAL] = { 0, };

static void
gum_thread_registry_class_init (GumThreadRegistryClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gum_thread_registry_dispose;
  object_class->finalize = gum_thread_registry_finalize;

  gum_thread_registry_signals[THREAD_ADDED] = g_signal_new ("thread-added",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      g_cclosure_marshal_VOID__BOXED, G_TYPE_NONE, 1, GUM_TYPE_THREAD_DETAILS);
  gum_thread_registry_signals[THREAD_RENAMED] = g_signal_new ("thread-renamed",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 2, GUM_TYPE_THREAD_DETAILS, G_TYPE_STRING);
  gum_thread_registry_signals[THREAD_REMOVED] = g_signal_new ("thread-removed",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      g_cclosure_marshal_VOID__BOXED, G_TYPE_NONE, 1, GUM_TYPE_THREAD_DETAILS);
}

static void
gum_thread_registry_init (GumThreadRegistry * self)
{
  g_rec_mutex_init (&self->mutex);
  self->state = GUM_THREAD_REGISTRY_CREATED;
  self->threads =
      g_ptr_array_new_full (8, (GDestroyNotify) gum_thread_details_free);

  GUM_THREAD_REGISTRY_LOCK (self);

  _gum_thread_registry_activate (self);
  self->state = GUM_THREAD_REGISTRY_ACTIVATED;

  GUM_THREAD_REGISTRY_UNLOCK (self);
}

static void
gum_thread_registry_dispose (GObject * object)
{
  GumThreadRegistry * self = GUM_THREAD_REGISTRY (object);

  GUM_THREAD_REGISTRY_LOCK (self);

  _gum_thread_registry_deactivate (self);

  GUM_THREAD_REGISTRY_UNLOCK (self);

  G_OBJECT_CLASS (gum_thread_registry_parent_class)->dispose (object);
}

static void
gum_thread_registry_finalize (GObject * object)
{
  GumThreadRegistry * self = GUM_THREAD_REGISTRY (object);

  g_ptr_array_unref (self->threads);
  g_rec_mutex_clear (&self->mutex);

  G_OBJECT_CLASS (gum_thread_registry_parent_class)->finalize (object);
}

GumThreadRegistry *
gum_thread_registry_obtain (void)
{
  static gsize cached_result = 0;

  if (g_once_init_enter (&cached_result))
  {
    GumThreadRegistry * registry;

    registry = g_object_new (GUM_TYPE_THREAD_REGISTRY, NULL);

    _gum_register_destructor (gum_deinit_thread_registry);

    g_once_init_leave (&cached_result, GPOINTER_TO_SIZE (registry));
  }

  return GSIZE_TO_POINTER (cached_result);
}

static void
gum_deinit_thread_registry (void)
{
  g_object_unref (gum_thread_registry_obtain ());
}

void
gum_thread_registry_enumerate_threads (GumThreadRegistry * self,
                                       GumFoundThreadFunc func,
                                       gpointer user_data)
{
  guint n, i;

  GUM_THREAD_REGISTRY_LOCK (self);

  n = self->threads->len;
  for (i = 0; i != n; i++)
  {
    GumThreadDetails * thread = g_ptr_array_index (self->threads, i);

    if (gum_cloak_has_thread (thread->id))
      continue;

    if (!func (thread, user_data))
      break;
  }

  GUM_THREAD_REGISTRY_UNLOCK (self);
}

void
gum_thread_registry_lock (GumThreadRegistry * self)
{
  GUM_THREAD_REGISTRY_LOCK (self);
}

void
gum_thread_registry_unlock (GumThreadRegistry * self)
{
  GUM_THREAD_REGISTRY_UNLOCK (self);
}

void
_gum_thread_registry_reset (GumThreadRegistry * self)
{
  GUM_THREAD_REGISTRY_LOCK (self);

  g_ptr_array_remove_range (self->threads, 0, self->threads->len);

  GUM_THREAD_REGISTRY_UNLOCK (self);
}

void
_gum_thread_registry_register (GumThreadRegistry * self,
                               const GumThreadDetails * thread)
{
  gboolean being_observed;

  GUM_THREAD_REGISTRY_LOCK (self);

  being_observed = self->state != GUM_THREAD_REGISTRY_CREATED;

  g_ptr_array_add (self->threads, gum_thread_details_copy (thread));

  GUM_THREAD_REGISTRY_UNLOCK (self);

  if (being_observed && !gum_cloak_has_thread (thread->id))
    g_signal_emit (self, gum_thread_registry_signals[THREAD_ADDED], 0, thread);
}

void
_gum_thread_registry_rename (GumThreadRegistry * self,
                             GumThreadId id,
                             const gchar * name)
{
  gboolean being_observed;
  GumThreadDetails * thread;
  gchar * previous_name;
  guint i;

  GUM_THREAD_REGISTRY_LOCK (self);

  being_observed = self->state != GUM_THREAD_REGISTRY_CREATED;

  thread = NULL;
  previous_name = NULL;
  for (i = 0; i != self->threads->len; i++)
  {
    GumThreadDetails * candidate = g_ptr_array_index (self->threads, i);

    if (candidate->id == id)
    {
      previous_name = g_strdup (candidate->name);

      thread = g_slice_dup (GumThreadDetails, candidate);
      thread->name = g_strdup (name);
      g_ptr_array_remove_index (self->threads, i);
      g_ptr_array_insert (self->threads, i, thread);

      thread = gum_thread_details_copy (thread);
      break;
    }
  }
  g_assert (thread != NULL);

  GUM_THREAD_REGISTRY_UNLOCK (self);

  if (!gum_cloak_has_thread (id))
    g_signal_emit (self, gum_thread_registry_signals[THREAD_RENAMED], 0,
        thread, previous_name);

  gum_thread_details_free (thread);
}

void
_gum_thread_registry_unregister (GumThreadRegistry * self,
                                 GumThreadId id)
{
  gboolean being_observed;
  GumThreadDetails * thread;
  guint i;

  GUM_THREAD_REGISTRY_LOCK (self);

  being_observed = self->state != GUM_THREAD_REGISTRY_CREATED;

  thread = NULL;
  for (i = 0; i != self->threads->len; i++)
  {
    GumThreadDetails * candidate = g_ptr_array_index (self->threads, i);

    if (candidate->id == id)
    {
      thread = g_ptr_array_steal_index (self->threads, i);
      break;
    }
  }
  g_assert (thread != NULL);

  GUM_THREAD_REGISTRY_UNLOCK (self);

  if (being_observed && !gum_cloak_has_thread (thread->id))
    g_signal_emit (self, gum_thread_registry_signals[THREAD_REMOVED], 0,
        thread);

  gum_thread_details_free (thread);
}
