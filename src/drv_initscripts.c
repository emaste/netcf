/*
 * drv_initscripts.c: the initscripts backend for netcf
 *
 * Copyright (C) 2009 Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: David Lutterkort <lutter@redhat.com>
 */

#include <config.h>
#include <internal.h>

#include <augeas.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "safe-alloc.h"
#include "ref.h"
#include "list.h"
#include "dutil.h"
#include "dutil_linux.h"

#include <libxml/parser.h>
#include <libxml/relaxng.h>
#include <libxml/tree.h>
#include <libxslt/xslt.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/transform.h>

static const char *const ifcfg_path =
    "/files/etc/sysconfig/network-scripts/*";

/* Augeas should only load the files we are interested in */
static const struct augeas_pv augeas_xfm[] = {
    /* Ifcfg files */
    { "/augeas/load/Ifcfg/lens", "Shellvars.lns" },
    { "/augeas/load/Ifcfg/incl",
      "/etc/sysconfig/network-scripts/ifcfg-*" },
    { "/augeas/load/Ifcfg/excl[1]", "*.augnew" },
    { "/augeas/load/Ifcfg/excl[2]", "*.augsave" },
    { "/augeas/load/Ifcfg/excl[3]", "*.rpmsave" },
    { "/augeas/load/Ifcfg/excl[4]", "*.rpmnew" },
    { "/augeas/load/Ifcfg/excl[5]", "*~" },
    /* iptables config */
    { "/augeas/load/Iptables/lens", "Iptables.lns" },
    { "/augeas/load/Iptables/incl", "/etc/sysconfig/iptables" },
    /* modprobe config */
    { "/augeas/load/Modprobe/lens", "Modprobe.lns" },
    { "/augeas/load/Modprobe/incl[1]", "/etc/modprobe.d/*" },
    { "/augeas/load/Modprobe/incl[2]", "/etc/modprobe.conf" },
    { "/augeas/load/Modprobe/excl[1]", "*.augnew" },
    { "/augeas/load/Modprobe/excl[2]", "*.augsave" },
    { "/augeas/load/Modprobe/excl[3]", "*.rpmsave" },
    { "/augeas/load/Modprobe/excl[4]", "*.rpmnew" },
    { "/augeas/load/Modprobe/excl[5]", "*~" },
    /* lokkit */
    { "/augeas/load/Lokkit/lens", "Lokkit.lns" },
    { "/augeas/load/Lokkit/incl", "/etc/sysconfig/system-config-firewall" },
    /* sysfs (choice entries from /class/net) */
    { "/augeas/load/Sysfs/lens", "Netcf.id" },
    { "/augeas/load/Sysfs/incl", "/sys/class/net/*/address" }
};

static const char *const prog_lokkit = "/usr/sbin/lokkit";
static const char *const lokkit_custom_rules =
    "--custom-rules=ipv4:filter:" DATADIR "/netcf/iptables-forward-bridged";

static const char *const prog_rc_d_iptables = "/etc/init.d/iptables";

/* Entries in a ifcfg file that tell us that the interface
 * is not a toplevel interface
 */
static const char *const subif_paths[] = {
    "MASTER", "BRIDGE"
};

static int is_slave(struct netcf *ncf, const char *intf) {
    for (int s = 0; s < ARRAY_CARDINALITY(subif_paths); s++) {
        int r;
        r = aug_fmt_match(ncf, NULL, "%s/%s", intf, subif_paths[s]);
        if (r != 0)
            return r;
    }
    return 0;
}

static int list_interfaces(struct netcf *ncf, char ***intf) {
    int nint = 0, result = 0;
    struct augeas *aug = NULL;

    aug = get_augeas(ncf);
    ERR_BAIL(ncf);

    /* Look in augeas for all interfaces */
    nint = aug_match(aug, ifcfg_path, intf);
    ERR_COND_BAIL(nint < 0, ncf, EOTHER);
    result = nint;

    /* Filter out the interfaces that are slaves/subordinate */
    for (int i = 0; i < result;) {
        if (is_slave(ncf, (*intf)[i])) {
            FREE((*intf)[i]);
            memmove(*intf + i, *intf + i + 1,
                    (nint - (i + 1))*sizeof((*intf)[0]));
            result -= 1;
        } else {
            i += 1;
        }
    }
    return result;
 error:
    free_matches(nint, intf);
    return -1;
}

/* Ensure we have an iptables rule to bridge physdevs. We take care of both
 * systems using iptables directly, and systems using lokkit (even if it's
 * only installed, but not used)
 */
static void bridge_physdevs(struct netcf *ncf) {
    struct augeas *aug = NULL;
    char *path = NULL, *p = NULL;
    const char *argv[5];
    int have_lokkit, use_lokkit;
    int r, nmatches;

    MEMZERO(argv, ARRAY_CARDINALITY(argv));

    aug = get_augeas(ncf);
    ERR_BAIL(ncf);

    defnode(ncf, "iptables", NULL, "/files/etc/sysconfig/iptables");
    ERR_BAIL(ncf);

    nmatches = aug_match(aug,
      "$iptables/table[ . = 'filter']/*[. = 'FORWARD'][match = 'physdev']", NULL);
    ERR_THROW(nmatches < 0, ncf, EOTHER, "failed to look for bridge");
    if (nmatches > 0)
        return;

    have_lokkit = access(prog_lokkit, X_OK) == 0;
    use_lokkit = aug_match(aug,
      "$iptables/#comment[. = 'Firewall configuration written by system-config-firewall']", NULL);
    ERR_THROW(use_lokkit < 0, ncf, EOTHER, "failed to look for lokkit");

    if (have_lokkit) {
        const char *rules_file = strrchr(lokkit_custom_rules, ':') + 1;
        int created;

        defnode(ncf, "fw", NULL, "/files/etc/sysconfig/system-config-firewall");
        ERR_BAIL(ncf);

        created = defnode(ncf, "fw_custom", rules_file,
                          "$fw/custom-rules[. = '%s']", rules_file);
        ERR_BAIL(ncf);

        if (created) {
            r = aug_set(aug, "$fw_custom", rules_file);
            ERR_COND_BAIL(r < 0, ncf, EOTHER);

            r = aug_set(aug, "$fw_custom/type", "ipv4");
            ERR_COND_BAIL(r < 0, ncf, EOTHER);
            FREE(p);

            r = aug_set(aug, "$fw_custom/table", "filter");
            ERR_COND_BAIL(r < 0, ncf, EOTHER);

            FREE(p);

            r = aug_save(aug);
            ERR_COND_BAIL(r < 0, ncf, EOTHER);
        }
        FREE(path);

        if (use_lokkit) {
            argv[0] = prog_lokkit;
            argv[1] = "--update";
            r = run_program(ncf, argv);
            ERR_BAIL(ncf);
        }
    }

    if (! use_lokkit) {
        defnode(ncf, "ipt_filter", NULL, "$iptables/table[. = 'filter']");
        ERR_BAIL(ncf);

        nmatches = aug_match(aug, "$ipt_filter", NULL);
        ERR_COND_BAIL(nmatches < 0, ncf, EOTHER);
        if (nmatches == 0) {
            r = aug_set(aug, "$ipt_filter", "filter");
            ERR_COND_BAIL(r < 0, ncf, EOTHER);
            r = aug_set(aug, "$ipt_filter/chain[1]", "INPUT");
            ERR_COND_BAIL(r < 0, ncf, EOTHER);
            r = aug_set(aug, "$ipt_filter/chain[1]/policy", "ACCEPT");
            ERR_COND_BAIL(r < 0, ncf, EOTHER);
            r = aug_set(aug, "$ipt_filter/chain[2]", "FORWARD");
            ERR_COND_BAIL(r < 0, ncf, EOTHER);
            r = aug_set(aug, "$ipt_filter/chain[2]/policy", "ACCEPT");
            ERR_COND_BAIL(r < 0, ncf, EOTHER);
            r = aug_set(aug, "$ipt_filter/chain[3]", "OUTPUT");
            ERR_COND_BAIL(r < 0, ncf, EOTHER);
            r = aug_set(aug, "$ipt_filter/chain[3]/policy", "ACCEPT");
            ERR_COND_BAIL(r < 0, ncf, EOTHER);
        } else {
            r = aug_insert(aug, "$ipt_filter/chain[last()]", "append", 0);
            ERR_COND_BAIL(r < 0, ncf, EOTHER);
        }
        r = aug_set(aug, "$ipt_filter/append[1]", "FORWARD");
        r = aug_set(aug, "$ipt_filter/append[1]/match", "physdev");
        ERR_COND_BAIL(r < 0, ncf, EOTHER);
        r = aug_set(aug, "$ipt_filter/append[1]/physdev-is-bridged", NULL);
        ERR_COND_BAIL(r < 0, ncf, EOTHER);
        r = aug_set(aug, "$ipt_filter/append[1]/jump", "ACCEPT");
        ERR_COND_BAIL(r < 0, ncf, EOTHER);

        r = aug_save(aug);
        ERR_COND_BAIL(r < 0, ncf, EOTHER);

        argv[0] = prog_rc_d_iptables;
        argv[1] = "condrestart";
        r = run_program(ncf, argv);
        ERR_BAIL(ncf);
    }
 error:
    free(path);
    free(p);
    return;
}

int drv_init(struct netcf *ncf) {
    int r;

    if (ALLOC(ncf->driver) < 0)
        return -1;

    ncf->driver->ioctl_fd = -1;
    ncf->driver->augeas_xfm_size = ARRAY_CARDINALITY(augeas_xfm);
    ncf->driver->augeas_xfm = augeas_xfm;

    // FIXME: Check for errors
    xsltInit();
    r = xslt_ext_register();
    ERR_THROW(r < 0, ncf, EINTERNAL, "xsltRegisterExtModule failed");
    ncf->driver->get = parse_stylesheet(ncf, "initscripts-get.xsl");
    ncf->driver->put = parse_stylesheet(ncf, "initscripts-put.xsl");
    ncf->driver->rng = rng_parse(ncf, "interface.rng");
    /* We undconditionally bridge physdevs; could be more discriminating */
    bridge_physdevs(ncf);

    /* open a socket for interface ioctls */
    ncf->driver->ioctl_fd = init_ioctl_fd(ncf);
    if (ncf->driver->ioctl_fd < 0)
        goto error;
    return 0;

 error:
    if (ncf->driver->ioctl_fd >= 0)
        close(ncf->driver->ioctl_fd);
    FREE(ncf->driver);
    return -1;
}

void drv_close(struct netcf *ncf) {
    xsltFreeStylesheet(ncf->driver->get);
    xsltFreeStylesheet(ncf->driver->put);
    xslt_ext_unregister();
    xsltCleanupGlobals();
    if (ncf->driver->ioctl_fd >= 0)
        close(ncf->driver->ioctl_fd);
    aug_close(ncf->driver->augeas);
    FREE(ncf->driver);
}

void drv_entry(struct netcf *ncf) {
    ncf->driver->load_augeas = 1;
}

static int list_interface_ids(struct netcf *ncf,
                              int maxnames, char **names,
                              unsigned int flags,
                              const char *id_attr) {
    struct augeas *aug = NULL;
    int nint = 0, nmatches = 0, nqualified = 0, result = 0, r;
    char **intf = NULL, **matches = NULL;

    aug = get_augeas(ncf);
    ERR_BAIL(ncf);
    nint = list_interfaces(ncf, &intf);
    if (!names) {
        maxnames = nint;    /* if not returning list, ignore maxnames too */
    }
    for (result = 0; (result < nint) && (nqualified < maxnames); result++) {
        nmatches = aug_fmt_match(ncf, &matches,
                                 "%s/%s", intf[result], id_attr);
        if (nmatches > 0) {
            const char *name;
            int is_qualified = ((flags & (NETCF_IFACE_ACTIVE|NETCF_IFACE_INACTIVE))
                                == (NETCF_IFACE_ACTIVE|NETCF_IFACE_INACTIVE));

            r = aug_get(aug, matches[nmatches-1], &name);
            ERR_COND_BAIL(r < 0, ncf, EOTHER);

            if (!is_qualified) {
                int if_is_active = is_active(ncf, name);
                if ((if_is_active && (flags & NETCF_IFACE_ACTIVE))
                    || ((!if_is_active) && (flags & NETCF_IFACE_INACTIVE))) {

                    is_qualified = 1;
                }
            }

            if (is_qualified) {
                if (names) {
                    names[nqualified] = strdup(name);
                    ERR_COND_BAIL(names[nqualified] == NULL, ncf, ENOMEM);
                }
                nqualified++;
            }
        }
        free_matches(nmatches, &matches);
    }
    free_matches(nint, &intf);
    return nqualified;
 error:
    free_matches(nmatches, &matches);
    free_matches(nint, &intf);
    return -1;
}

int drv_list_interfaces(struct netcf *ncf, int maxnames, char **names, unsigned int flags) {
    return list_interface_ids(ncf, maxnames, names, flags, "DEVICE");
}

int drv_num_of_interfaces(struct netcf *ncf, unsigned int flags) {
    return list_interface_ids(ncf, 0, NULL, flags, "DEVICE");
}

struct netcf_if *drv_lookup_by_name(struct netcf *ncf, const char *name) {
    struct netcf_if *nif = NULL;
    char *pathx = NULL;
    char *name_dup = NULL;
    struct augeas *aug;
    int nint;
    int r;

    aug = get_augeas(ncf);
    ERR_BAIL(ncf);

    r = xasprintf(&pathx, "%s[DEVICE = '%s']", ifcfg_path, name);
    ERR_COND_BAIL(r < 0, ncf, ENOMEM);

    nint = aug_get(aug, pathx, NULL);
    ERR_COND_BAIL(nint < 0, ncf, EOTHER);

    if (nint == 0 || is_slave(ncf, pathx))
        goto done;

    name_dup = strdup(name);
    ERR_COND_BAIL(name_dup == NULL, ncf, ENOMEM);

    nif = make_netcf_if(ncf, name_dup);
    ERR_BAIL(ncf);
    goto done;

 error:
    unref(nif, netcf_if);
    FREE(name_dup);
 done:
    FREE(pathx);
    return nif;
}

/* Get an XML desription of the interfaces (just paths, really) in INTF.
 * The format is a very simple representation of the Augeas tree (see
 * xml/augeas.rng)
 */
static xmlDocPtr aug_get_xml(struct netcf *ncf, int nint, char **intf) {
    struct augeas *aug;
    xmlDocPtr result = NULL;
    xmlNodePtr root = NULL, tree = NULL;
    char **matches = NULL;
    int nmatches, r;

    aug = get_augeas(ncf);
    ERR_BAIL(ncf);

    result = xmlNewDoc(BAD_CAST "1.0");
    root = xmlNewNode(NULL, BAD_CAST "forest");
    xmlDocSetRootElement(result, root);

    for (int i=0; i < nint; i++) {
        tree = xmlNewChild(root, NULL, BAD_CAST "tree", NULL);
        xmlNewProp(tree, BAD_CAST "path", BAD_CAST intf[i]);
        nmatches = aug_fmt_match(ncf, &matches, "%s/%s", intf[i], "*");
        ERR_COND_BAIL(nint < 0, ncf, EOTHER);
        for (int j = 0; j < nmatches; j++) {
            xmlNodePtr node = xmlNewChild(tree, NULL, BAD_CAST "node", NULL);
            const char *value;
            xmlNewProp(node, BAD_CAST "label",
                       BAD_CAST matches[j] + strlen(intf[i]) + 1);
            r = aug_get(aug, matches[j], &value);
            ERR_COND_BAIL(r < 0, ncf, EOTHER);
            xmlNewProp(node, BAD_CAST "value", BAD_CAST value);
        }
        free_matches(nmatches, &matches);
    }

    return result;

 error:
    free_matches(nmatches, &matches);
    xmlFreeDoc(result);
    return NULL;
}

/* Write the XML doc in the simple Augeas format into the Augeas tree */
static int aug_put_xml(struct netcf *ncf, xmlDocPtr xml) {
    xmlNodePtr forest;
    char *path = NULL, *lpath = NULL, *label = NULL, *value = NULL;
    struct augeas *aug = NULL;
    int result = -1;
    int r;

    aug = get_augeas(ncf);
    ERR_BAIL(ncf);

    forest = xmlDocGetRootElement(xml);
    ERR_THROW(forest == NULL, ncf, EINTERNAL, "missing root element");
    ERR_THROW(! xmlStrEqual(forest->name, BAD_CAST "forest"), ncf,
              EINTERNAL, "expected root node labeled 'forest', not '%s'",
              forest->name);
    list_for_each(tree, forest->children) {
        ERR_THROW(! xmlStrEqual(tree->name, BAD_CAST "tree"), ncf,
                  EINTERNAL, "expected node labeled 'tree', not '%s'",
                  tree->name);
        path = xml_prop(tree, "path");
        int toplevel = 1;
        /* This is a little drastic, since it clears out the file entirely */
        r = aug_rm(aug, path);
        ERR_THROW(r < 0, ncf, EINTERNAL, "aug_rm of '%s' failed", path);
        list_for_each(node, tree->children) {
            label = xml_prop(node, "label");
            value = xml_prop(node, "value");
            /* We should mark the toplevel interface from the XSLT */
            if (STREQ(label, "BRIDGE") || STREQ(label, "MASTER")) {
                toplevel = 0;
            }
            r = xasprintf(&lpath, "%s/%s", path, label);
            ERR_THROW(r < 0, ncf, ENOMEM, NULL);

            r = aug_set(aug, lpath, value);
            ERR_THROW(r < 0, ncf, EOTHER,
                      "aug_set of '%s' failed", lpath);
            FREE(lpath);
            xmlFree(label);
            xmlFree(value);
            label = value = NULL;
        }
        xmlFree(path);
        path = NULL;
    }
    result = 0;
 error:
    xmlFree(label);
    xmlFree(value);
    xmlFree(path);
    FREE(lpath);
    return result;
}

char *drv_xml_desc(struct netcf_if *nif) {
    char *result = NULL;
    struct augeas *aug;
    struct netcf *ncf;
    char **intf = NULL;
    xmlDocPtr aug_xml = NULL, ncf_xml = NULL;
    int nint = 0;

    ncf = nif->ncf;
    aug = get_augeas(ncf);
    ERR_BAIL(ncf);

    nint = aug_fmt_match(ncf, &intf,
              "%s[ DEVICE = '%s' or BRIDGE = '%s' or MASTER = '%s']",
              ifcfg_path, nif->name, nif->name, nif->name);
    ERR_BAIL(ncf);

    aug_xml = aug_get_xml(ncf, nint, intf);
    ncf_xml = xsltApplyStylesheet(ncf->driver->put, aug_xml, NULL);

    xmlDocDumpFormatMemory(ncf_xml, (xmlChar **) &result, NULL, 1);

 done:
    free_matches(nint, &intf);
    xmlFreeDoc(aug_xml);
    xmlFreeDoc(ncf_xml);
    return result;
 error:
    FREE(result);
    goto done;
}

/* Get the content of /interface/@name. Result must be freed with xmlFree() */
static char *device_name_from_xml(xmlDocPtr xml) {
    xmlNodePtr iface;
    char *result;

    iface = xmlDocGetRootElement(xml);
    if (iface == NULL) return NULL;

    result = xml_prop(iface, "name");
    return result;
}

/* The device NAME is a bond if it is mentioned as the MASTER in sopme
 * other devices config file
 */
static bool is_bond(struct netcf *ncf, const char *name) {
    int nmatches = 0;

    nmatches = aug_fmt_match(ncf, NULL,
                             "%s[ MASTER = '%s']", ifcfg_path, name);
    return nmatches > 0;
}

/* The device NAME is a bridge if it has an entry TYPE=Bridge */
static bool is_bridge(struct netcf *ncf, const char *name) {
    int nmatches = 0;

    nmatches = aug_fmt_match(ncf, NULL,
                             "%s[ DEVICE = '%s' and TYPE = 'Bridge']",
                             ifcfg_path, name);
    return nmatches > 0;
}

static int bridge_slaves(struct netcf *ncf, const char *name, char ***slaves) {
    struct augeas *aug = NULL;
    int r, nslaves = 0;

    aug = get_augeas(ncf);
    ERR_BAIL(ncf);

    nslaves = aug_fmt_match(ncf, slaves,
                            "%s[ BRIDGE = '%s' ]/DEVICE", ifcfg_path, name);
    ERR_BAIL(ncf);
    for (int i=0; i < nslaves; i++) {
        char *p = (*slaves)[i];
        const char *dev;
        r = aug_get(aug, p, &dev);
        ERR_COND_BAIL(r < 0, ncf, EOTHER);

        (*slaves)[i] = strdup(dev);
        free(p);
        ERR_COND_BAIL(slaves[i] == NULL, ncf, ENOMEM);
    }
    return nslaves;
 error:
    free_matches(nslaves, slaves);
    return -1;
}

struct netcf_if *drv_define(struct netcf *ncf, const char *xml_str) {
    xmlDocPtr ncf_xml = NULL, aug_xml = NULL;
    char *name = NULL, *path = NULL;
    struct netcf_if *result = NULL;
    int r;
    struct augeas *aug = get_augeas(ncf);

    ncf_xml = parse_xml(ncf, xml_str);
    ERR_BAIL(ncf);

    rng_validate(ncf, ncf_xml);
    ERR_BAIL(ncf);

    name = device_name_from_xml(ncf_xml);
    ERR_COND_BAIL(name == NULL, ncf, EINTERNAL);

    /* Clean out existing definitions */
    r = xasprintf(&path,
          "%s[ DEVICE = '%s' or BRIDGE = '%s' or MASTER = '%s']",
          ifcfg_path, name, name, name);
    ERR_COND_BAIL(r < 0, ncf, ENOMEM);

    r = aug_rm(aug, path);
    ERR_COND_BAIL(r < 0, ncf, EOTHER);

    // FIXME: Check for errors from ApplyStylesheet
    aug_xml = xsltApplyStylesheet(ncf->driver->get, ncf_xml, NULL);

    aug_put_xml(ncf, aug_xml);
    ERR_BAIL(ncf);

    if (is_bond(ncf, name)) {
        modprobed_alias_bond(ncf, name);
        ERR_BAIL(ncf);
    }

    r = aug_save(aug);
    ERR_THROW(r < 0, ncf, EOTHER, "aug_save failed");

    result = make_netcf_if(ncf, name);
    ERR_BAIL(ncf);

 done:
    free(path);
    xmlFreeDoc(ncf_xml);
    xmlFreeDoc(aug_xml);
    return result;
 error:
    unref(result, netcf_if);
    goto done;
}

int drv_undefine(struct netcf_if *nif) {
    struct augeas *aug = NULL;
    struct netcf *ncf = nif->ncf;
    int r;
    char *path = NULL;

    aug = get_augeas(ncf);
    ERR_BAIL(ncf);

    if (is_bond(ncf, nif->name)) {
        modprobed_unalias_bond(ncf, nif->name);
        ERR_BAIL(ncf);
    }

    r = xasprintf(&path,
          "%s[ DEVICE = '%s' or BRIDGE = '%s' or MASTER = '%s']",
          ifcfg_path, nif->name, nif->name, nif->name);
    ERR_COND_BAIL(r < 0, ncf, ENOMEM);

    r = aug_rm(aug, path);
    ERR_COND_BAIL(r < 0, ncf, EOTHER);

    r = aug_save(aug);
    ERR_COND_BAIL(r < 0, ncf, EOTHER);

    FREE(path);
    return 0;
 error:
    FREE(path);
    return -1;
}

int drv_lookup_by_mac_string(struct netcf *ncf, const char *mac,
                             int maxifaces, struct netcf_if **ifaces)
{
    struct augeas *aug = NULL;
    char *path = NULL, *ifcfg = NULL;
    const char **names = NULL;
    int nmatches = 0;
    char **matches = NULL;
    int r;
    int result = -1;

    MEMZERO(ifaces, maxifaces);

    aug = get_augeas(ncf);
    ERR_BAIL(ncf);

    nmatches = aug_match_mac(ncf, mac, &matches);
    ERR_THROW(nmatches < 0, ncf, EOTHER, "looking up %s failed", mac);
    if (nmatches == 0) {
        result = 0;
        goto done;
    }

    r = ALLOC_N(names, nmatches);
    ERR_COND_BAIL(r < 0, ncf, ENOMEM);

    int cnt = 0;
    for (int i = 0; i < nmatches; i++) {
        r = xasprintf(&ifcfg, "%s[DEVICE = '%s']", ifcfg_path, matches[i]);
        ERR_COND_BAIL(r < 0, ncf, ENOMEM);

        if (! is_slave(ncf, ifcfg))
            names[cnt++] = matches[i];
        FREE(ifcfg);
    }
    for (int i=0; i < cnt && i < maxifaces; i++) {
        char *name = strdup(names[i]);
        ERR_COND_BAIL(name == NULL, ncf, ENOMEM);
        ifaces[i] = make_netcf_if(ncf, name);
        ERR_BAIL(ncf);
    }
    result = cnt;
    goto done;

 error:
    for (int i=0; i < maxifaces; i++)
        unref(ifaces[i], netcf_if);
 done:
    free(names);
    free(ifcfg);
    free(path);
    free_matches(nmatches, &matches);
    return result;
}

const char *drv_mac_string(struct netcf_if *nif) {
    struct netcf *ncf = nif->ncf;
    const char *mac;
    char *path = NULL;
    int r;

    r = aug_get_mac(ncf, nif->name, &mac);
    ERR_THROW(r <= 0, ncf, EOTHER, "could not lookup MAC of %s", nif->name);

    if (nif->mac == NULL || STRNEQ(nif->mac, mac)) {
        FREE(nif->mac);
        nif->mac = strdup(mac);
        ERR_COND_BAIL(nif->mac == NULL, ncf, ENOMEM);
    }
    /* fallthrough intentional */
 error:
    FREE(path);
    return nif->mac;
}

/*
 * Bringing interfaces up/down
 */

int drv_if_up(struct netcf_if *nif) {
    static const char *const ifup = "ifup";
    struct netcf *ncf = nif->ncf;
    char **slaves = NULL;
    int nslaves = 0;
    int result = -1;

    if (is_bridge(ncf, nif->name)) {
        /* Bring up bridge slaves before the bridge */
        nslaves = bridge_slaves(ncf, nif->name, &slaves);
        ERR_BAIL(ncf);

        for (int i=0; i < nslaves; i++) {
            run1(ncf, ifup, slaves[i]);
            ERR_BAIL(ncf);
        }
    }
    run1(ncf, ifup, nif->name);
    result = 0;
 error:
    free_matches(nslaves, &slaves);
    return result;
}

int drv_if_down(struct netcf_if *nif) {
    static const char *const ifdown = "ifdown";
    struct netcf *ncf = nif->ncf;
    char **slaves = NULL;
    int nslaves = 0;
    int result = -1;

    run1(ncf, ifdown, nif->name);
    if (is_bridge(ncf, nif->name)) {
        /* Bring up bridge slaves after the bridge */
        nslaves = bridge_slaves(ncf, nif->name, &slaves);
        ERR_BAIL(ncf);

        for (int i=0; i < nslaves; i++) {
            run1(ncf, ifdown, slaves[i]);
            ERR_BAIL(ncf);
        }
    }
    result = 0;
 error:
    free_matches(nslaves, &slaves);
    return result;
}

/*
 * Test interface
 */
int drv_get_aug(struct netcf *ncf, const char *ncf_xml, char **aug_xml) {
    /* Use utility implementation */
    return dutil_get_aug(ncf, ncf_xml, aug_xml);
}

/* Transform the Augeas XML AUG_XML into interface XML NCF_XML */
int drv_put_aug(struct netcf *ncf, const char *aug_xml, char **ncf_xml) {
    /* Use utility implementation */
    return dutil_put_aug(ncf, aug_xml, ncf_xml);
}

/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
/* vim: set ts=4 sw=4 et: */
