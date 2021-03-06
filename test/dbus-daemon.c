/* Integration tests for the dbus-daemon
 *
 * Author: Simon McVittie <simon.mcvittie@collabora.co.uk>
 * Copyright © 2010-2011 Nokia Corporation
 * Copyright © 2015 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <config.h>

#include <errno.h>
#include <string.h>

#include <dbus/dbus.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "test-utils-glib.h"

#include <string.h>

#ifdef DBUS_UNIX
# include <unistd.h>
# include <sys/types.h>

# ifdef HAVE_GIO_UNIX
    /* The CMake build system doesn't know how to check for this yet */
#   include <gio/gunixfdmessage.h>
# endif
#endif

/* Platforms where we know that credentials-passing passes both the
 * uid and the pid. Please keep these in alphabetical order.
 *
 * These platforms should #error in _dbus_read_credentials_socket()
 * if we didn't detect their flavour of credentials-passing, since that
 * would be a regression.
 */
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || \
  defined(__linux__) || \
  defined(__NetBSD__) || \
  defined(__OpenBSD__)
# define UNIX_USER_SHOULD_WORK
# define PID_SHOULD_WORK
#endif

/* Platforms where we know that credentials-passing passes the
 * uid, but not necessarily the pid. Again, alphabetical order please.
 *
 * These platforms should also #error in _dbus_read_credentials_socket()
 * if we didn't detect their flavour of credentials-passing.
 */
#if 0 /* defined(__your_platform_here__) */
# define UNIX_USER_SHOULD_WORK
#endif

typedef struct {
    gboolean skip;

    TestMainContext *ctx;

    DBusError e;
    GError *ge;

    GPid daemon_pid;
    gchar *address;

    DBusConnection *left_conn;

    DBusConnection *right_conn;
    GQueue held_messages;
    gboolean right_conn_echo;
    gboolean right_conn_hold;
    gboolean wait_forever_called;

    gchar *tmp_runtime_dir;
    gchar *saved_runtime_dir;
} Fixture;

static DBusHandlerResult
echo_filter (DBusConnection *connection,
    DBusMessage *message,
    void *user_data)
{
  Fixture *f = user_data;
  DBusMessage *reply;

  if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_METHOD_CALL)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  /* WaitForever() never replies, emulating a service that has got stuck */
  if (dbus_message_is_method_call (message, "com.example", "WaitForever"))
    {
      f->wait_forever_called = TRUE;
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  reply = dbus_message_new_method_return (message);

  if (reply == NULL)
    g_error ("OOM");

  if (!dbus_connection_send (connection, reply, NULL))
    g_error ("OOM");

  dbus_message_unref (reply);

  return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
hold_filter (DBusConnection *connection,
    DBusMessage *message,
    void *user_data)
{
  Fixture *f = user_data;

  if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_METHOD_CALL)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  g_queue_push_tail (&f->held_messages, dbus_message_ref (message));

  return DBUS_HANDLER_RESULT_HANDLED;
}

typedef struct {
    const char *bug_ref;
    guint min_messages;
    const char *config_file;
    enum { SPECIFY_ADDRESS = 0, RELY_ON_DEFAULT } connect_mode;
} Config;

static void
setup (Fixture *f,
    gconstpointer context)
{
  const Config *config = context;

  f->ctx = test_main_context_get ();
  f->ge = NULL;
  dbus_error_init (&f->e);

  if (config != NULL && config->connect_mode == RELY_ON_DEFAULT)
    {
      /* this is chosen to be something needing escaping */
      f->tmp_runtime_dir = g_dir_make_tmp ("dbus=daemon=test.XXXXXX", &f->ge);
      g_assert_no_error (f->ge);

      /* we're relying on being single-threaded for this to be safe */
      f->saved_runtime_dir = g_strdup (g_getenv ("XDG_RUNTIME_DIR"));
      g_setenv ("XDG_RUNTIME_DIR", f->tmp_runtime_dir, TRUE);
    }

  f->address = test_get_dbus_daemon (config ? config->config_file : NULL,
                                     TEST_USER_ME,
                                     &f->daemon_pid);

  if (f->address == NULL)
    {
      f->skip = TRUE;
      return;
    }

  f->left_conn = test_connect_to_bus (f->ctx, f->address);

  if (config != NULL && config->connect_mode == RELY_ON_DEFAULT)
    {
      /* use the default bus for the echo service ("right"), to check that
       * it ends up on the same bus as the client ("left") */
      f->right_conn = dbus_bus_get_private (DBUS_BUS_SESSION, &f->e);
      test_assert_no_error (&f->e);

      if (!test_connection_setup (f->ctx, f->right_conn))
        g_error ("OOM");
    }
  else
    {
      f->right_conn = test_connect_to_bus (f->ctx, f->address);
    }
}

static void
add_echo_filter (Fixture *f)
{
  if (!dbus_connection_add_filter (f->right_conn, echo_filter, f, NULL))
    g_error ("OOM");

  f->right_conn_echo = TRUE;
}

static void
add_hold_filter (Fixture *f)
{
  if (!dbus_connection_add_filter (f->right_conn, hold_filter, f, NULL))
    g_error ("OOM");

  f->right_conn_hold = TRUE;
}

static void
pc_count (DBusPendingCall *pc,
    void *data)
{
  guint *received_p = data;

  (*received_p)++;
}

static void
pc_enqueue (DBusPendingCall *pc,
            void *data)
{
  GQueue *q = data;
  DBusMessage *m = dbus_pending_call_steal_reply (pc);

  g_test_message ("message of type %d", dbus_message_get_type (m));
  g_queue_push_tail (q, m);
}

static void
test_echo (Fixture *f,
    gconstpointer context)
{
  const Config *config = context;
  guint count = 2000;
  guint sent;
  guint received = 0;
  double elapsed;

  if (f->skip)
    return;

  if (config != NULL && config->bug_ref != NULL)
    g_test_bug (config->bug_ref);

  if (g_test_perf ())
    count = 100000;

  if (config != NULL)
    count = MAX (config->min_messages, count);

  add_echo_filter (f);

  g_test_timer_start ();

  for (sent = 0; sent < count; sent++)
    {
      DBusMessage *m = dbus_message_new_method_call (
          dbus_bus_get_unique_name (f->right_conn), "/",
          "com.example", "Spam");
      DBusPendingCall *pc;

      if (m == NULL)
        g_error ("OOM");

      if (!dbus_connection_send_with_reply (f->left_conn, m, &pc,
                                            DBUS_TIMEOUT_INFINITE) ||
          pc == NULL)
        g_error ("OOM");

      if (dbus_pending_call_get_completed (pc))
        pc_count (pc, &received);
      else if (!dbus_pending_call_set_notify (pc, pc_count, &received,
            NULL))
        g_error ("OOM");

      dbus_pending_call_unref (pc);
      dbus_message_unref (m);
    }

  while (received < count)
    test_main_context_iterate (f->ctx, TRUE);

  elapsed = g_test_timer_elapsed ();

  g_test_maximized_result (count / elapsed, "%u messages / %f seconds",
      count, elapsed);
}

static void
test_no_reply (Fixture *f,
    gconstpointer context)
{
  const Config *config = context;
  DBusMessage *m;
  DBusPendingCall *pc;
  DBusMessage *reply = NULL;
  enum { TIMEOUT, DISCONNECT } mode;
  gboolean ok;

  if (f->skip)
    return;

  g_test_bug ("76112");

  if (config != NULL && config->config_file != NULL)
    mode = TIMEOUT;
  else
    mode = DISCONNECT;

  m = dbus_message_new_method_call (
      dbus_bus_get_unique_name (f->right_conn), "/",
      "com.example", "WaitForever");

  add_echo_filter (f);

  if (m == NULL)
    g_error ("OOM");

  if (!dbus_connection_send_with_reply (f->left_conn, m, &pc,
                                        DBUS_TIMEOUT_INFINITE) ||
      pc == NULL)
    g_error ("OOM");

  if (dbus_pending_call_get_completed (pc))
    test_pending_call_store_reply (pc, &reply);
  else if (!dbus_pending_call_set_notify (pc, test_pending_call_store_reply,
        &reply, NULL))
    g_error ("OOM");

  dbus_pending_call_unref (pc);
  dbus_message_unref (m);

  if (mode == DISCONNECT)
    {
      while (!f->wait_forever_called)
        test_main_context_iterate (f->ctx, TRUE);

      dbus_connection_remove_filter (f->right_conn, echo_filter, f);
      dbus_connection_close (f->right_conn);
      dbus_connection_unref (f->right_conn);
      f->right_conn = NULL;
    }

  while (reply == NULL)
    test_main_context_iterate (f->ctx, TRUE);

  /* using inefficient string comparison for better assertion message */
  g_assert_cmpstr (
      dbus_message_type_to_string (dbus_message_get_type (reply)), ==,
      dbus_message_type_to_string (DBUS_MESSAGE_TYPE_ERROR));
  ok = dbus_set_error_from_message (&f->e, reply);
  g_assert (ok);
  g_assert_cmpstr (f->e.name, ==, DBUS_ERROR_NO_REPLY);

  if (mode == DISCONNECT)
    g_assert_cmpstr (f->e.message, ==,
        "Message recipient disconnected from message bus without replying");
  else
    g_assert_cmpstr (f->e.message, ==,
        "Message did not receive a reply (timeout by message bus)");
}

static void
test_creds (Fixture *f,
    gconstpointer context)
{
  const char *unique = dbus_bus_get_unique_name (f->left_conn);
  DBusMessage *m = dbus_message_new_method_call (DBUS_SERVICE_DBUS,
      DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionCredentials");
  DBusPendingCall *pc;
  DBusMessageIter args_iter;
  DBusMessageIter arr_iter;
  DBusMessageIter pair_iter;
  DBusMessageIter var_iter;
  enum {
      SEEN_UNIX_USER = 1,
      SEEN_PID = 2,
      SEEN_WINDOWS_SID = 4,
      SEEN_LINUX_SECURITY_LABEL = 8
  } seen = 0;

  if (m == NULL)
    g_error ("OOM");

  if (!dbus_message_append_args (m,
        DBUS_TYPE_STRING, &unique,
        DBUS_TYPE_INVALID))
    g_error ("OOM");

  if (!dbus_connection_send_with_reply (f->left_conn, m, &pc,
                                        DBUS_TIMEOUT_USE_DEFAULT) ||
      pc == NULL)
    g_error ("OOM");

  dbus_message_unref (m);
  m = NULL;

  if (dbus_pending_call_get_completed (pc))
    test_pending_call_store_reply (pc, &m);
  else if (!dbus_pending_call_set_notify (pc, test_pending_call_store_reply,
                                          &m, NULL))
    g_error ("OOM");

  while (m == NULL)
    test_main_context_iterate (f->ctx, TRUE);

  g_assert_cmpstr (dbus_message_get_signature (m), ==, "a{sv}");

  dbus_message_iter_init (m, &args_iter);
  g_assert_cmpuint (dbus_message_iter_get_arg_type (&args_iter), ==,
      DBUS_TYPE_ARRAY);
  g_assert_cmpuint (dbus_message_iter_get_element_type (&args_iter), ==,
      DBUS_TYPE_DICT_ENTRY);
  dbus_message_iter_recurse (&args_iter, &arr_iter);

  while (dbus_message_iter_get_arg_type (&arr_iter) != DBUS_TYPE_INVALID)
    {
      const char *name;

      dbus_message_iter_recurse (&arr_iter, &pair_iter);
      g_assert_cmpuint (dbus_message_iter_get_arg_type (&pair_iter), ==,
          DBUS_TYPE_STRING);
      dbus_message_iter_get_basic (&pair_iter, &name);
      dbus_message_iter_next (&pair_iter);
      g_assert_cmpuint (dbus_message_iter_get_arg_type (&pair_iter), ==,
          DBUS_TYPE_VARIANT);
      dbus_message_iter_recurse (&pair_iter, &var_iter);

      if (g_strcmp0 (name, "UnixUserID") == 0)
        {
#ifdef G_OS_UNIX
          guint32 u32;

          g_assert (!(seen & SEEN_UNIX_USER));
          g_assert_cmpuint (dbus_message_iter_get_arg_type (&var_iter), ==,
              DBUS_TYPE_UINT32);
          dbus_message_iter_get_basic (&var_iter, &u32);
          g_test_message ("%s of this process is %u", name, u32);
          g_assert_cmpuint (u32, ==, geteuid ());
          seen |= SEEN_UNIX_USER;
#else
          g_assert_not_reached ();
#endif
        }
      else if (g_strcmp0 (name, "WindowsSID") == 0)
        {
#ifdef G_OS_WIN32
          gchar *sid;
          char *self_sid;

          g_assert (!(seen & SEEN_WINDOWS_SID));
          g_assert_cmpuint (dbus_message_iter_get_arg_type (&var_iter), ==,
              DBUS_TYPE_STRING);
          dbus_message_iter_get_basic (&var_iter, &sid);
          g_test_message ("%s of this process is %s", name, sid);
          if (_dbus_getsid (&self_sid, 0))
            {
              g_assert_cmpstr (self_sid, ==, sid);
              LocalFree(self_sid);
            }
          seen |= SEEN_WINDOWS_SID;
#else
          g_assert_not_reached ();
#endif
        }
      else if (g_strcmp0 (name, "ProcessID") == 0)
        {
          guint32 u32;

          g_assert (!(seen & SEEN_PID));
          g_assert_cmpuint (dbus_message_iter_get_arg_type (&var_iter), ==,
              DBUS_TYPE_UINT32);
          dbus_message_iter_get_basic (&var_iter, &u32);
          g_test_message ("%s of this process is %u", name, u32);
#ifdef G_OS_UNIX
          g_assert_cmpuint (u32, ==, getpid ());
#elif defined(G_OS_WIN32)
          g_assert_cmpuint (u32, ==, GetCurrentProcessId ());
#else
          g_assert_not_reached ();
#endif
          seen |= SEEN_PID;
        }
      else if (g_strcmp0 (name, "LinuxSecurityLabel") == 0)
        {
#ifdef __linux__
          gchar *label;
          int len;
          DBusMessageIter ay_iter;

          g_assert (!(seen & SEEN_LINUX_SECURITY_LABEL));
          g_assert_cmpuint (dbus_message_iter_get_arg_type (&var_iter), ==,
              DBUS_TYPE_ARRAY);
          dbus_message_iter_recurse (&var_iter, &ay_iter);
          g_assert_cmpuint (dbus_message_iter_get_arg_type (&ay_iter), ==,
              DBUS_TYPE_BYTE);
          dbus_message_iter_get_fixed_array (&ay_iter, &label, &len);
          g_test_message ("%s of this process is %s", name, label);
          g_assert_cmpuint (strlen (label) + 1, ==, len);
          seen |= SEEN_LINUX_SECURITY_LABEL;
#else
          g_assert_not_reached ();
#endif
        }

      dbus_message_iter_next (&arr_iter);
    }

#ifdef UNIX_USER_SHOULD_WORK
  g_assert (seen & SEEN_UNIX_USER);
#endif

#ifdef PID_SHOULD_WORK
  g_assert (seen & SEEN_PID);
#endif

#ifdef G_OS_WIN32
  g_assert (seen & SEEN_WINDOWS_SID);
#endif
}

static void
test_processid (Fixture *f,
    gconstpointer context)
{
  const char *unique = dbus_bus_get_unique_name (f->left_conn);
  DBusMessage *m = dbus_message_new_method_call (DBUS_SERVICE_DBUS,
      DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetConnectionUnixProcessID");
  DBusPendingCall *pc;
  DBusError error = DBUS_ERROR_INIT;
  guint32 pid;

  if (m == NULL)
    g_error ("OOM");

  if (!dbus_message_append_args (m,
        DBUS_TYPE_STRING, &unique,
        DBUS_TYPE_INVALID))
    g_error ("OOM");

  if (!dbus_connection_send_with_reply (f->left_conn, m, &pc,
                                        DBUS_TIMEOUT_USE_DEFAULT) ||
      pc == NULL)
    g_error ("OOM");

  dbus_message_unref (m);
  m = NULL;

  if (dbus_pending_call_get_completed (pc))
    test_pending_call_store_reply (pc, &m);
  else if (!dbus_pending_call_set_notify (pc, test_pending_call_store_reply,
                                          &m, NULL))
    g_error ("OOM");

  while (m == NULL)
    test_main_context_iterate (f->ctx, TRUE);

  if (dbus_set_error_from_message (&error, m))
    {
      g_assert_cmpstr (error.name, ==, DBUS_ERROR_UNIX_PROCESS_ID_UNKNOWN);

#ifdef PID_SHOULD_WORK
      g_error ("Expected pid to be passed, but got %s: %s",
          error.name, error.message);
#endif

      dbus_error_free (&error);
    }
  else if (dbus_message_get_args (m, &error,
        DBUS_TYPE_UINT32, &pid,
        DBUS_TYPE_INVALID))
    {
      g_assert_cmpstr (dbus_message_get_signature (m), ==, "u");
      test_assert_no_error (&error);

      g_test_message ("GetConnectionUnixProcessID returned %u", pid);

#ifdef G_OS_UNIX
      g_assert_cmpuint (pid, ==, getpid ());
#elif defined(G_OS_WIN32)
      g_assert_cmpuint (pid, ==, GetCurrentProcessId ());
#else
      g_assert_not_reached ();
#endif
    }
  else
    {
      g_error ("Unexpected error: %s: %s", error.name, error.message);
    }
}

static void
test_canonical_path_uae (Fixture *f,
    gconstpointer context)
{
  DBusMessage *m = dbus_message_new_method_call (DBUS_SERVICE_DBUS,
      DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "UpdateActivationEnvironment");
  DBusPendingCall *pc;
  DBusMessageIter args_iter;
  DBusMessageIter arr_iter;

  if (m == NULL)
    g_error ("OOM");

  dbus_message_iter_init_append (m, &args_iter);

  /* Append an empty a{ss} (string => string dictionary). */
  if (!dbus_message_iter_open_container (&args_iter, DBUS_TYPE_ARRAY,
        "{ss}", &arr_iter) ||
      !dbus_message_iter_close_container (&args_iter, &arr_iter))
    g_error ("OOM");

  if (!dbus_connection_send_with_reply (f->left_conn, m, &pc,
                                        DBUS_TIMEOUT_USE_DEFAULT) ||
      pc == NULL)
    g_error ("OOM");

  dbus_message_unref (m);
  m = NULL;

  if (dbus_pending_call_get_completed (pc))
    test_pending_call_store_reply (pc, &m);
  else if (!dbus_pending_call_set_notify (pc, test_pending_call_store_reply,
                                          &m, NULL))
    g_error ("OOM");

  while (m == NULL)
    test_main_context_iterate (f->ctx, TRUE);

  /* it succeeds */
  g_assert_cmpint (dbus_message_get_type (m), ==,
      DBUS_MESSAGE_TYPE_METHOD_RETURN);

  dbus_message_unref (m);

  /* Now try with the wrong object path */
  m = dbus_message_new_method_call (DBUS_SERVICE_DBUS,
      "/com/example/Wrong", DBUS_INTERFACE_DBUS, "UpdateActivationEnvironment");

  if (m == NULL)
    g_error ("OOM");

  dbus_message_iter_init_append (m, &args_iter);

  /* Append an empty a{ss} (string => string dictionary). */
  if (!dbus_message_iter_open_container (&args_iter, DBUS_TYPE_ARRAY,
        "{ss}", &arr_iter) ||
      !dbus_message_iter_close_container (&args_iter, &arr_iter))
    g_error ("OOM");

  if (!dbus_connection_send_with_reply (f->left_conn, m, &pc,
                                        DBUS_TIMEOUT_USE_DEFAULT) ||
      pc == NULL)
    g_error ("OOM");

  dbus_message_unref (m);
  m = NULL;

  if (dbus_pending_call_get_completed (pc))
    test_pending_call_store_reply (pc, &m);
  else if (!dbus_pending_call_set_notify (pc, test_pending_call_store_reply,
                                          &m, NULL))
    g_error ("OOM");

  while (m == NULL)
    test_main_context_iterate (f->ctx, TRUE);

  /* it fails, yielding an error message with one string argument */
  g_assert_cmpint (dbus_message_get_type (m), ==, DBUS_MESSAGE_TYPE_ERROR);
  g_assert_cmpstr (dbus_message_get_error_name (m), ==,
      DBUS_ERROR_ACCESS_DENIED);
  g_assert_cmpstr (dbus_message_get_signature (m), ==, "s");

  dbus_message_unref (m);
}

static void
test_max_connections (Fixture *f,
    gconstpointer context)
{
  DBusError error = DBUS_ERROR_INIT;
  DBusConnection *third_conn;
  DBusConnection *failing_conn;

  if (f->skip)
    return;

  /* We have two connections already */
  g_assert (f->left_conn != NULL);
  g_assert (f->right_conn != NULL);

  /* Our configuration file sets the limit to 3 connections, either globally
   * or per uid, so this one is the last that will work */
  third_conn = test_connect_to_bus (f->ctx, f->address);

  /* This one is going to fail. We don't guarantee whether it will fail
   * now, or while registering (implementation detail: it's the latter). */
  failing_conn = dbus_connection_open_private (f->address, &error);

  if (failing_conn != NULL)
    {
      gboolean ok = dbus_bus_register (failing_conn, &error);

      g_assert (!ok);
    }

  g_assert (dbus_error_is_set (&error));
  g_assert_cmpstr (error.name, ==, DBUS_ERROR_LIMITS_EXCEEDED);

  if (failing_conn != NULL)
    {
      dbus_connection_close (failing_conn);
      dbus_connection_unref (failing_conn);
    }

  dbus_connection_close (third_conn);
  dbus_connection_unref (third_conn);
}

static void
test_max_replies_per_connection (Fixture *f,
    gconstpointer context)
{
  GQueue received = G_QUEUE_INIT;
  GQueue errors = G_QUEUE_INIT;
  DBusMessage *m;
  DBusPendingCall *pc;
  guint i;
  DBusError error = DBUS_ERROR_INIT;

  if (f->skip)
    return;

  add_hold_filter (f);

  /* The configured limit is 3 replies per connection. */
  for (i = 0; i < 3; i++)
    {
      m = dbus_message_new_method_call (
          dbus_bus_get_unique_name (f->right_conn), "/",
          "com.example", "Spam");

      if (m == NULL)
        g_error ("OOM");

      if (!dbus_connection_send_with_reply (f->left_conn, m, &pc,
                                            DBUS_TIMEOUT_INFINITE) ||
          pc == NULL)
        g_error ("OOM");

      if (dbus_pending_call_get_completed (pc))
        pc_enqueue (pc, &received);
      else if (!dbus_pending_call_set_notify (pc, pc_enqueue, &received,
            NULL))
        g_error ("OOM");

      dbus_pending_call_unref (pc);
      dbus_message_unref (m);
    }

  while (g_queue_get_length (&f->held_messages) < 3)
    test_main_context_iterate (f->ctx, TRUE);

  g_assert_cmpuint (g_queue_get_length (&received), ==, 0);
  g_assert_cmpuint (g_queue_get_length (&errors), ==, 0);

  /* Go a couple of messages over the limit. */
  for (i = 0; i < 2; i++)
    {
      m = dbus_message_new_method_call (
          dbus_bus_get_unique_name (f->right_conn), "/",
          "com.example", "Spam");

      if (m == NULL)
        g_error ("OOM");

      if (!dbus_connection_send_with_reply (f->left_conn, m, &pc,
                                            DBUS_TIMEOUT_INFINITE) ||
          pc == NULL)
        g_error ("OOM");

      if (dbus_pending_call_get_completed (pc))
        pc_enqueue (pc, &errors);
      else if (!dbus_pending_call_set_notify (pc, pc_enqueue, &errors,
            NULL))
        g_error ("OOM");

      dbus_pending_call_unref (pc);
      dbus_message_unref (m);
    }

  /* Reply to the held messages. */
  for (m = g_queue_pop_head (&f->held_messages);
       m != NULL;
       m = g_queue_pop_head (&f->held_messages))
    {
      DBusMessage *reply = dbus_message_new_method_return (m);

      if (reply == NULL)
        g_error ("OOM");

      if (!dbus_connection_send (f->right_conn, reply, NULL))
        g_error ("OOM");

      dbus_message_unref (reply);
    }

  /* Wait for all 5 replies to come in. */
  while (g_queue_get_length (&received) + g_queue_get_length (&errors) < 5)
    test_main_context_iterate (f->ctx, TRUE);

  /* The first three succeeded. */
  for (i = 0; i < 3; i++)
    {
      m = g_queue_pop_head (&received);
      g_assert (m != NULL);

      if (dbus_set_error_from_message (&error, m))
        g_error ("Unexpected error: %s: %s", error.name, error.message);

      dbus_message_unref (m);
    }

  /* The last two failed. */
  for (i = 0; i < 2; i++)
    {
      m = g_queue_pop_head (&errors);
      g_assert (m != NULL);

      if (!dbus_set_error_from_message (&error, m))
        g_error ("Unexpected success");

      g_assert_cmpstr (error.name, ==, DBUS_ERROR_LIMITS_EXCEEDED);
      dbus_error_free (&error);
      dbus_message_unref (m);
    }

  g_assert_cmpuint (g_queue_get_length (&received), ==, 0);
  g_assert_cmpuint (g_queue_get_length (&errors), ==, 0);
  g_queue_clear (&received);
  g_queue_clear (&errors);
}

static void
test_max_match_rules_per_connection (Fixture *f,
    gconstpointer context)
{
  DBusError error = DBUS_ERROR_INIT;

  if (f->skip)
    return;

  dbus_bus_add_match (f->left_conn, "sender='com.example.C1'", &error);
  test_assert_no_error (&error);
  dbus_bus_add_match (f->left_conn, "sender='com.example.C2'", &error);
  test_assert_no_error (&error);
  dbus_bus_add_match (f->left_conn, "sender='com.example.C3'", &error);
  test_assert_no_error (&error);

  dbus_bus_add_match (f->left_conn, "sender='com.example.C4'", &error);
  g_assert_cmpstr (error.name, ==, DBUS_ERROR_LIMITS_EXCEEDED);
  dbus_error_free (&error);

  dbus_bus_remove_match (f->left_conn, "sender='com.example.C3'", &error);
  test_assert_no_error (&error);

  dbus_bus_add_match (f->left_conn, "sender='com.example.C4'", &error);
  test_assert_no_error (&error);
}

static void
test_max_names_per_connection (Fixture *f,
    gconstpointer context)
{
  DBusError error = DBUS_ERROR_INIT;
  int ret;

  if (f->skip)
    return;

  /* The limit in the configuration file is set to 4, but we only own 3
   * names here - remember that the unique name is a name too. */

  ret = dbus_bus_request_name (f->left_conn, "com.example.C1", 0, &error);
  test_assert_no_error (&error);
  g_assert_cmpint (ret, ==, DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);

  ret = dbus_bus_request_name (f->left_conn, "com.example.C2", 0, &error);
  test_assert_no_error (&error);
  g_assert_cmpint (ret, ==, DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);

  ret = dbus_bus_request_name (f->left_conn, "com.example.C3", 0, &error);
  test_assert_no_error (&error);
  g_assert_cmpint (ret, ==, DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);

  ret = dbus_bus_request_name (f->left_conn, "com.example.C4", 0, &error);
  g_assert_cmpstr (error.name, ==, DBUS_ERROR_LIMITS_EXCEEDED);
  dbus_error_free (&error);
  g_assert_cmpint (ret, ==, -1);

  ret = dbus_bus_release_name (f->left_conn, "com.example.C3", &error);
  test_assert_no_error (&error);
  g_assert_cmpint (ret, ==, DBUS_RELEASE_NAME_REPLY_RELEASED);

  ret = dbus_bus_request_name (f->left_conn, "com.example.C4", 0, &error);
  test_assert_no_error (&error);
  g_assert_cmpint (ret, ==, DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER);
}

#if defined(DBUS_UNIX) && defined(HAVE_UNIX_FD_PASSING) && defined(HAVE_GIO_UNIX)

static DBusHandlerResult
wait_for_disconnected_cb (DBusConnection *client_conn,
    DBusMessage *message,
    void *data)
{
  gboolean *disconnected = data;

  if (dbus_message_is_signal (message, DBUS_INTERFACE_LOCAL, "Disconnected"))
    {
      *disconnected = TRUE;
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const guchar partial_message[] =
{
  DBUS_LITTLE_ENDIAN,
  DBUS_MESSAGE_TYPE_METHOD_CALL,
  0, /* flags */
  1, /* version */
  0xff, 0xff, 0, 0, /* length of body = 65535 bytes */
  1, 2, 3, 4, /* cookie */
  0xff, 0xff, 0, 0, /* length of header fields array = 65535 bytes */
  42 /* pretending to be the beginning of the header fields array */
};

static void
send_all_with_fd (GSocket *socket,
                  const guchar *local_partial_message,
                  gsize len,
                  int fd)
{
  GSocketControlMessage *fdm = g_unix_fd_message_new ();
  GError *error = NULL;
  gssize sent;
  GOutputVector vector = { local_partial_message, len };

  g_unix_fd_message_append_fd (G_UNIX_FD_MESSAGE (fdm), fd, &error);
  g_assert_no_error (error);

  sent = g_socket_send_message (socket, NULL, &vector, 1, &fdm, 1,
      G_SOCKET_MSG_NONE, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (sent, >=, 1);
  g_assert_cmpint (sent, <=, vector.size);

  while (((gsize) sent) < vector.size)
    {
      vector.size -= sent;
      vector.buffer = ((const guchar *) vector.buffer) + sent;
      sent = g_socket_send_message (socket, NULL, &vector, 1, NULL, 0,
          G_SOCKET_MSG_NONE, NULL, &error);
      g_assert_no_error (error);
      g_assert_cmpint (sent, >=, 1);
      g_assert_cmpint (sent, <=, vector.size);
    }

  g_object_unref (fdm);
}

static void
test_pending_fd_timeout (Fixture *f,
    gconstpointer context)
{
  GError *error = NULL;
  gint64 start;
  int fd;
  GSocket *socket;
  gboolean have_mem;
  gboolean disconnected = FALSE;

  if (f->skip)
    return;

  have_mem = dbus_connection_add_filter (f->left_conn, wait_for_disconnected_cb,
      &disconnected, NULL);
  g_assert (have_mem);

  /* This is not API. Never do this. */

  if (!dbus_connection_get_socket (f->left_conn, &fd))
    g_error ("failed to steal fd from left connection");

  socket = g_socket_new_from_fd (fd, &error);
  g_assert_no_error (error);
  g_assert (socket != NULL);

  /* We send part of a message that contains a fd, then stop. */
  start = g_get_monotonic_time ();
  send_all_with_fd (socket, partial_message, G_N_ELEMENTS (partial_message),
                    fd);

  while (!disconnected)
    {
      test_progress ('.');
      test_main_context_iterate (f->ctx, TRUE);

      /* It should take no longer than 500ms to get disconnected. We'll
       * be generous and allow 1000ms. */
      g_assert_cmpint (g_get_monotonic_time (), <=, start + G_USEC_PER_SEC);
    }

  g_object_unref (socket);
}

#endif

static void
teardown (Fixture *f,
    gconstpointer context G_GNUC_UNUSED)
{
  dbus_error_free (&f->e);
  g_clear_error (&f->ge);

  if (f->left_conn != NULL)
    {
      dbus_connection_close (f->left_conn);
      dbus_connection_unref (f->left_conn);
      f->left_conn = NULL;
    }

  if (f->right_conn != NULL)
    {
      if (f->right_conn_echo)
        {
          dbus_connection_remove_filter (f->right_conn, echo_filter, f);
          f->right_conn_echo = FALSE;
        }

      if (f->right_conn_hold)
        {
          dbus_connection_remove_filter (f->right_conn, hold_filter, f);
          f->right_conn_hold = FALSE;
        }

      g_queue_foreach (&f->held_messages, (GFunc) dbus_message_unref, NULL);
      g_queue_clear (&f->held_messages);

      dbus_connection_close (f->right_conn);
      dbus_connection_unref (f->right_conn);
      f->right_conn = NULL;
    }

  if (f->daemon_pid != 0)
    {
      test_kill_pid (f->daemon_pid);
      g_spawn_close_pid (f->daemon_pid);
      f->daemon_pid = 0;
    }

  if (f->tmp_runtime_dir != NULL)
    {
      gchar *path;

      /* the socket may exist */
      path = g_strdup_printf ("%s/bus", f->tmp_runtime_dir);
      g_assert (g_remove (path) == 0 || errno == ENOENT);
      g_free (path);
      /* there shouldn't be anything else in there */
      g_assert_cmpint (g_rmdir (f->tmp_runtime_dir), ==, 0);

      /* we're relying on being single-threaded for this to be safe */
      if (f->saved_runtime_dir != NULL)
        g_setenv ("XDG_RUNTIME_DIR", f->saved_runtime_dir, TRUE);
      else
        g_unsetenv ("XDG_RUNTIME_DIR");
      g_free (f->saved_runtime_dir);
      g_free (f->tmp_runtime_dir);
    }

  test_main_context_unref (f->ctx);
  g_free (f->address);
}

static Config limited_config = {
    "34393", 10000, "valid-config-files/incoming-limit.conf",
    SPECIFY_ADDRESS
};

static Config finite_timeout_config = {
    NULL, 1, "valid-config-files/finite-timeout.conf",
    SPECIFY_ADDRESS
};

#ifdef DBUS_UNIX
static Config listen_unix_runtime_config = {
    "61303", 1, "valid-config-files/listen-unix-runtime.conf",
    RELY_ON_DEFAULT
};
#endif

static Config max_completed_connections_config = {
    NULL, 1, "valid-config-files/max-completed-connections.conf",
    SPECIFY_ADDRESS
};

static Config max_connections_per_user_config = {
    NULL, 1, "valid-config-files/max-connections-per-user.conf",
    SPECIFY_ADDRESS
};

static Config max_replies_per_connection_config = {
    NULL, 1, "valid-config-files/max-replies-per-connection.conf",
    SPECIFY_ADDRESS
};

static Config max_match_rules_per_connection_config = {
    NULL, 1, "valid-config-files/max-match-rules-per-connection.conf",
    SPECIFY_ADDRESS
};

static Config max_names_per_connection_config = {
    NULL, 1, "valid-config-files/max-names-per-connection.conf",
    SPECIFY_ADDRESS
};

#if defined(DBUS_UNIX) && defined(HAVE_UNIX_FD_PASSING) && defined(HAVE_GIO_UNIX)
static Config pending_fd_timeout_config = {
    NULL, 1, "valid-config-files/pending-fd-timeout.conf",
    SPECIFY_ADDRESS
};
#endif

int
main (int argc,
    char **argv)
{
  test_init (&argc, &argv);

  g_test_add ("/echo/session", Fixture, NULL, setup, test_echo, teardown);
  g_test_add ("/echo/limited", Fixture, &limited_config,
      setup, test_echo, teardown);
  g_test_add ("/no-reply/disconnect", Fixture, NULL,
      setup, test_no_reply, teardown);
  g_test_add ("/no-reply/timeout", Fixture, &finite_timeout_config,
      setup, test_no_reply, teardown);
  g_test_add ("/creds", Fixture, NULL, setup, test_creds, teardown);
  g_test_add ("/processid", Fixture, NULL, setup, test_processid, teardown);
  g_test_add ("/canonical-path/uae", Fixture, NULL,
      setup, test_canonical_path_uae, teardown);
  g_test_add ("/limits/max-completed-connections", Fixture,
      &max_completed_connections_config,
      setup, test_max_connections, teardown);
  g_test_add ("/limits/max-connections-per-user", Fixture,
      &max_connections_per_user_config,
      setup, test_max_connections, teardown);
  g_test_add ("/limits/max-replies-per-connection", Fixture,
      &max_replies_per_connection_config,
      setup, test_max_replies_per_connection, teardown);
  g_test_add ("/limits/max-match-rules-per-connection", Fixture,
      &max_match_rules_per_connection_config,
      setup, test_max_match_rules_per_connection, teardown);
  g_test_add ("/limits/max-names-per-connection", Fixture,
      &max_names_per_connection_config,
      setup, test_max_names_per_connection, teardown);

#if defined(DBUS_UNIX) && defined(HAVE_UNIX_FD_PASSING) && defined(HAVE_GIO_UNIX)
  g_test_add ("/limits/pending-fd-timeout", Fixture,
      &pending_fd_timeout_config,
      setup, test_pending_fd_timeout, teardown);
#endif

#ifdef DBUS_UNIX
  /* We can't test this in loopback.c with the rest of unix:runtime=yes,
   * because dbus_bus_get[_private] is the only way to use the default,
   * and that blocks on a round-trip to the dbus-daemon */
  g_test_add ("/unix-runtime-is-default", Fixture, &listen_unix_runtime_config,
      setup, test_echo, teardown);
#endif

  return g_test_run ();
}
