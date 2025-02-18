/*
 * Copyright (C) 2025 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumthreadregistry-priv.h"

#include <pthread/introspection.h>

static void gum_thread_registry_on_thread_event (unsigned int event,
    pthread_t thread, void * addr, size_t size);

static GumThreadRegistry * gum_registry;
static gboolean gum_hook_installed = FALSE;
static pthread_introspection_hook_t gum_previous_hook;

void
_gum_thread_registry_activate (GumThreadRegistry * self)
{
  gum_registry = self;
  gum_previous_hook =
      pthread_introspection_hook_install (gum_thread_registry_on_thread_event);
  gum_hook_installed = TRUE;
}

void
_gum_thread_registry_deactivate (GumThreadRegistry * self)
{
  if (gum_hook_installed)
  {
    (void) pthread_introspection_hook_install (gum_previous_hook);
    gum_previous_hook = NULL;
    gum_hook_installed = FALSE;
  }
}

static void
gum_thread_registry_on_thread_event (unsigned int event,
                                     pthread_t thread,
                                     void * addr,
                                     size_t size)
{
  switch (event)
  {
    case PTHREAD_INTROSPECTION_THREAD_START:
    {
      GumThreadDetails t;
      gchar name[64];

      t.id = pthread_mach_thread_np (thread);

      t.name = NULL;
      pthread_getname_np (thread, name, sizeof (name));
      if (name[0] != '\0')
        t.name = name;

      t.state = GUM_THREAD_RUNNING;

      bzero (&t.cpu_context, sizeof (GumCpuContext));

      _gum_thread_registry_register (gum_registry, &t);

      break;
    }
    case PTHREAD_INTROSPECTION_THREAD_TERMINATE:
    {
      _gum_thread_registry_unregister (gum_registry,
          pthread_mach_thread_np (thread));
      break;
    }
    default:
      break;
  }
}
