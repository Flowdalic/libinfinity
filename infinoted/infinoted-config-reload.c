/* libinfinity - a GObject-based infinote implementation
 * Copyright (C) 2007-2015 Armin Burgmeier <armin@arbur.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include <infinoted/infinoted-config-reload.h>
#include <infinoted/infinoted-dh-params.h>
#include <infinoted/infinoted-log.h>
#include <infinoted/infinoted-pam.h>

#include <libinfinity/server/infd-filesystem-storage.h>
#include <libinfinity/server/infd-filesystem-account-storage.h>
#include <libinfinity/inf-config.h>
#include <libinfinity/inf-i18n.h>

#include <string.h>

static const guint8 INFINOTED_CONFIG_RELOAD_IPV6_ANY_ADDR[16] =
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static void
infinoted_config_reload_update_connection_sasl_context(InfXmlConnection* xml,
                                                       gpointer userdata)
{
  if(!INF_IS_XMPP_CONNECTION(xml))
    return;

  inf_xmpp_connection_reset_sasl_authentication(
    INF_XMPP_CONNECTION(xml),
    (InfSaslContext*) userdata,
    userdata ? "PLAIN" : NULL
  );
}

/**
 * infinoted_config_reload:
 * @run: A #InfinotedRun.
 * @error: Location to store error information, if any.
 *
 * Reloads the server's config file(s) at runtime. If there is a problem
 * loading them the server is untouched, the function returns %FALSE and
 * @error is set.
 *
 * Returns: %TRUE on success or %FALSE if an error occured.
 */
gboolean
infinoted_config_reload(InfinotedRun* run,
                        GError** error)
{
  InfinotedStartup* startup;
  gnutls_dh_params_t dh_params;
  InfdTcpServer* tcp6;
  InfdTcpServer* tcp4;

  guint port;
  InfIpAddress* addr4;
  InfIpAddress* addr6;
  GError* local_error;

  InfdStorage* storage;
  InfdFilesystemStorage* filesystem_storage;
  InfdFilesystemAccountStorage* filesystem_account_storage;
  gchar* root_directory;
  gboolean result;

#ifdef G_OS_WIN32
  gchar* module_path;
#endif
  gchar* plugin_path;
  InfinotedPluginManager* plugin_manager;

  /* Note that this opens a new log handle to the log file. */
  startup = infinoted_startup_new(NULL, NULL, error);
  if(!startup) return FALSE;

  /* Acquire DH params if necessary (if security policy changed from
   * no-tls to one of allow-tls or require-tls). */
  dh_params = run->dh_params;
  if(startup->credentials)
  {
    result = infinoted_dh_params_ensure(
      startup->log,
      startup->credentials,
      &dh_params,
      error
    );

    if(!result)
    {
      infinoted_startup_free(startup);
      return FALSE;
    }
  }

  if((startup->options->listen_address != NULL &&
      run->startup->options->listen_address == NULL) ||
     (startup->options->listen_address == NULL &&
      run->startup->options->listen_address != NULL) ||
     (startup->options->listen_address != NULL &&
      run->startup->options->listen_address != NULL &&
      inf_ip_address_collate(startup->options->listen_address,
                             run->startup->options->listen_address) != 0))
  {
    g_set_error(
      error,
      g_quark_from_static_string("INFINOTED_CONFIG_RELOAD_ERROR"),
      0,
      _("Changing the listen address at runtime is not supported")
    );

    infinoted_startup_free(startup);
    return FALSE;
  }

  /* Find out the port we are currently running on */
  tcp4 = tcp6 = NULL;
  if(run->xmpp6)
    g_object_get(G_OBJECT(run->xmpp6), "tcp-server", &tcp6, NULL);
  if(run->xmpp4)
    g_object_get(G_OBJECT(run->xmpp4), "tcp-server", &tcp4, NULL);

  g_assert(tcp4 != NULL || tcp6 != NULL);
  if(tcp6) g_object_get(G_OBJECT(tcp6), "local-port", &port, NULL);
  else g_object_get(G_OBJECT(tcp4), "local-port", &port, NULL);

  if(tcp4) g_object_unref(tcp4);
  if(tcp6) g_object_unref(tcp6);
  tcp4 = tcp6 = NULL;

  /* If the port changes, then create new servers */
  if(startup->options->port != port)
  {
    /* TODO: This is the same logic as in infinoted_run_new()... should
     * probably go into an extra function. */
    if(startup->options->listen_address == NULL)
    {
      addr4 = NULL;
      addr6 = inf_ip_address_new_raw6(INFINOTED_CONFIG_RELOAD_IPV6_ANY_ADDR);
    }
    else
    {
      addr4 = startup->options->listen_address;
      addr6 = startup->options->listen_address;
    }

    tcp6 = g_object_new(
      INFD_TYPE_TCP_SERVER,
      "io", run->io,
      "local-address", addr6,
      "local-port", startup->options->port,
      NULL
    );

    if(startup->options->listen_address == NULL)
      inf_ip_address_free(addr6);

    if(!infd_tcp_server_bind(tcp6, NULL))
    {
      g_object_unref(tcp6);
      tcp6 = NULL;
    }

    tcp4 = g_object_new(
      INFD_TYPE_TCP_SERVER,
      "io", run->io,
      "local-address", addr4,
      "local-port", startup->options->port,
      NULL
    );

    local_error = NULL;
    if(!infd_tcp_server_bind(tcp4, &local_error))
    {
      g_object_unref(tcp4);
      tcp4 = NULL;

      if(tcp6 != NULL)
      {
        g_error_free(local_error);
      }
      else
      {
        g_propagate_error(error, local_error);
        infinoted_startup_free(startup);
        return FALSE;
      }
    }
  }

  /* Beyond this point, tcp4 or tcp6 are non-null if the port was changed and
   * the new server sockets could be bound successfully. */

  g_object_get(G_OBJECT(run->directory), "storage", &storage, NULL);
  g_assert(INFD_IS_FILESYSTEM_STORAGE(storage));
  filesystem_storage = INFD_FILESYSTEM_STORAGE(storage);
  g_object_get(
    G_OBJECT(filesystem_storage),
    "root-directory", &root_directory,
    NULL
  );

  g_object_unref(storage);
  filesystem_storage = NULL;

  filesystem_account_storage = NULL;
  if(strcmp(root_directory, startup->options->root_directory) != 0)
  {
    /* Root directory changes. I don't think this is actually useful, but
     * all code is there, so let's support it. */
    filesystem_storage =
      infd_filesystem_storage_new(startup->options->root_directory);
    filesystem_account_storage = infd_filesystem_account_storage_new();

    result = infd_filesystem_account_storage_set_filesystem(
      filesystem_account_storage,
      filesystem_storage,
      error
    );

    if(result == FALSE)
    {
      g_object_unref(filesystem_account_storage);
      g_object_unref(filesystem_storage);
      g_object_unref(plugin_manager);
      infinoted_startup_free(startup);
      return FALSE;
    }
  }

  /* This should be the last thing that may fail which we do, because we
   * allow connection on the new port after this. */
  if(tcp4 != NULL || tcp6 != NULL)
  {
    local_error = NULL;
    if(tcp6 != NULL)
    {
      local_error = NULL;
      if(!infd_tcp_server_open(tcp6, &local_error))
      {
        g_object_unref(tcp6);
        tcp6 = NULL;
      }
    }

    if(tcp4)
    {
      if(!infd_tcp_server_open(tcp4,
                               (local_error != NULL) ? NULL : &local_error))
      {
        g_object_unref(tcp4);
        tcp4 = NULL;
      }
    }

    if(tcp4 == NULL && tcp6 == NULL)
    {
      g_propagate_error(error, local_error);
      g_object_unref(plugin_manager);
      if(filesystem_storage) g_object_unref(filesystem_storage);
      if(filesystem_account_storage)
        g_object_unref(filesystem_account_storage);
      infinoted_startup_free(startup);
      return FALSE;
    }

    /* One of the server startups might have failed - that's not a problem if
     * we could start the other one. */
    if(local_error) g_error_free(local_error);
  }

  /* OK, so beyond this point there is nothing that can fail anymore. */

  if(tcp4 != NULL || tcp6 != NULL)
  {
    /* We have new servers, close old ones */
    if(run->xmpp6 != NULL)
    {
      infd_server_pool_remove_server(run->pool, INFD_XML_SERVER(run->xmpp6));
      infd_xml_server_close(INFD_XML_SERVER(run->xmpp6));
      g_object_unref(run->xmpp6);
      run->xmpp6 = NULL;
    }

    if(run->xmpp4 != NULL)
    {
      infd_server_pool_remove_server(run->pool, INFD_XML_SERVER(run->xmpp4));
      infd_xml_server_close(INFD_XML_SERVER(run->xmpp4));
      g_object_unref(run->xmpp4);
      run->xmpp4 = NULL;
    }

    if(tcp6 != NULL)
    {
      run->xmpp6 = infd_xmpp_server_new(
        tcp6,
        startup->options->security_policy,
        startup->credentials,
        NULL,
        NULL
      );

      g_object_unref(tcp6);

      infd_server_pool_add_server(run->pool, INFD_XML_SERVER(run->xmpp6));

#ifdef LIBINFINITY_HAVE_AVAHI
      infd_server_pool_add_local_publisher(
        run->pool,
        run->xmpp6,
        INF_LOCAL_PUBLISHER(run->avahi)
      );
#endif
    }

    if(tcp4 != NULL)
    {
      run->xmpp4 = infd_xmpp_server_new(
        tcp4,
        startup->options->security_policy,
        startup->credentials,
        NULL,
        NULL
      );

      g_object_unref(tcp4);

      infd_server_pool_add_server(run->pool, INFD_XML_SERVER(run->xmpp4));

#ifdef LIBINFINITY_HAVE_AVAHI
      infd_server_pool_add_local_publisher(
        run->pool,
        run->xmpp4,
        INF_LOCAL_PUBLISHER(run->avahi)
      );
#endif
    }
  }
  else
  {
    /* No new servers, so just set new certificate settings
     * for existing ones. */
    if(run->xmpp6 != NULL)
    {
      /* Make sure to set credentials before security-policy */
      g_object_set(
        G_OBJECT(run->xmpp6),
        "credentials", startup->credentials,
        "security-policy", startup->options->security_policy,
        NULL
      );
    }

    if(run->xmpp4 != NULL)
    {
      g_object_set(
        G_OBJECT(run->xmpp4),
        "credentials", startup->credentials,
        "security-policy", startup->options->security_policy,
        NULL
      );
    }
  }

  /* Now, re-initialize plugins. This is a bit tricky, because it can fail,
   * and because we need to unload the previous plugins first.
   *
   * TODO: It would be better if we only add or remove plugins that did not
   * exist before, and for the rest we call a _reload_params() function. That
   * function should be allowed to fail, and if it fails, the plugin is
   * unloaded.
   */

  /* TODO: Make sure this unloads all plugins... at the moment it wouldn't
   * happen if some plugin ref-ed the plugin manager. */
  g_assert(run->plugin_manager != NULL);
  g_object_unref(run->plugin_manager);

  /* TODO: Storage and account storage should not be updated if they have
   * been altered by a plugin... maybe the storage itself should be turned
   * into a plugin. */
  if(filesystem_storage != NULL)
  {
    g_object_set(
      G_OBJECT(run->directory),
      "storage", filesystem_storage,
      "account-storage", filesystem_account_storage,
      NULL
    );

    g_object_unref(filesystem_storage);
    g_object_unref(filesystem_account_storage);
  }

#ifdef G_OS_WIN32
  module_path = g_win32_get_package_installation_directory_of_module(NULL);
  plugin_path = g_build_filename(module_path, "lib", PLUGIN_PATH, NULL);
  g_free(module_path);
#else
  plugin_path = g_build_filename(PLUGIN_LIBPATH, PLUGIN_PATH, NULL);
#endif

  plugin_manager = infinoted_plugin_manager_new(
    run->directory,
    startup->log,
    startup->credentials
  );

  local_error = NULL;

  result = infinoted_plugin_manager_load(
    plugin_manager,
    plugin_path,
    (const gchar* const*)startup->options->plugins,
    startup->options->config_key_file,
    &local_error
  );

  g_free(plugin_path);
  infinoted_options_drop_config_file(startup->options);

  if(result == FALSE)
  {
    infinoted_log_error(
      startup->log,
      _("Failed to re-load plugins: %s"),
      local_error->message
    );

    infinoted_log_error(
      startup->log,
      _("Plugins are disabled. Please fix the problem and reload "
        "configuration again.")
    );

    g_error_free(local_error);
  }

  run->plugin_manager = plugin_manager;

#ifdef LIBINFINITY_HAVE_LIBDAEMON
  /* Remember whether we have been daemonized; this is not a config file
   * option, so not properly set in our newly created startup. */
  startup->options->daemonize = run->startup->options->daemonize;
#endif

  if(run->xmpp4 != NULL)
  {
    g_object_set(
      G_OBJECT(run->xmpp4),
      "sasl-context",    startup->sasl_context,
      "sasl-mechanisms", startup->sasl_context ? "PLAIN" : NULL,
      NULL
    );
  }

  if(run->xmpp6 != NULL)
  {
    g_object_set(
      G_OBJECT(run->xmpp6),
      "sasl-context",    startup->sasl_context,
      "sasl-mechanisms", startup->sasl_context ? "PLAIN" : NULL,
      NULL
    );
  }

  /* Give each connection the new sasl context. This is necessary even if the
   * connection already had a sasl context since that holds on to the old
   * startup object. This aborts authentications in progress and otherwise
   * has no effect, really. */
  infd_directory_foreach_connection(
    run->directory,
    infinoted_config_reload_update_connection_sasl_context,
    startup->sasl_context
  );

  infinoted_startup_free(run->startup);
  run->startup = startup;

  return TRUE;
}

/* vim:set et sw=2 ts=2: */
