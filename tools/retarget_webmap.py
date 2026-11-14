#!/usr/bin/env python3
"""
Point a web map's layers at a survey's NEW feature service, keeping symbology.

When Survey123 Connect has to recreate a survey's feature service (e.g. after
adding a repeat), the service gets a new name and item id. Web maps still point
at the old one: the layers keep their styling but draw nothing. Re-adding the
layer shows the data but loses the styling. This swaps the url and itemId on
the existing layers instead, so the styling, transparency, labels and pop-ups
all stay.

    python retarget_webmap.py                         dry run - shows what would change
    python retarget_webmap.py --write                 apply it

Options
    --webmap <item id>     web map to fix   (default: the DEW report web map)
    --service <name>       the NEW service name, as in its REST url
                           (default: the DEW survey's service from 28 Oct 2026)

Only layers whose url is a Survey123 service ("service_<32 hex>") other than
the new one are touched. The web map JSON is backed up to webmap_backups\\
before anything is written, every time.
"""

import json
import os
import re
import sys
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spray_stats as ss  # noqa: E402  (config, token, portal_get, PORTAL)
import requests            # noqa: E402

DEFAULT_WEBMAP = "e9b505e6c67f49b195a176229c98fca0"
DEFAULT_SERVICE = "service_3f88e9734da34216a824af6e8f100aac"

SVC_RE = re.compile(r"/services/(service_[0-9a-f]{32})(_[a-z]+)?/FeatureServer/(\d+)", re.I)


def arg(flag, default):
    a = sys.argv
    return a[a.index(flag) + 1] if flag in a and a.index(flag) + 1 < len(a) else default


def rest_json(url, token):
    r = requests.get(url, params={"f": "json", "token": token}, timeout=60)
    r.raise_for_status()
    body = r.json()
    if "error" in body:
        raise RuntimeError("%s: %s" % (url, body["error"].get("message", body["error"])))
    return body


def walk_layers(layers):
    """Operational layers, including those inside group layers."""
    for L in layers or []:
        yield L
        if L.get("layers"):
            yield from walk_layers(L["layers"])


def main():
    write = "--write" in sys.argv
    webmap = arg("--webmap", DEFAULT_WEBMAP)
    new_name = arg("--service", DEFAULT_SERVICE)

    cfg = ss.load_config()
    token = ss.get_token(cfg)

    item = ss.portal_get("content/items/%s" % webmap, token)
    data = ss.portal_get("content/items/%s/data" % webmap, token)
    owner = item.get("owner")
    print("\n  Web map: %s (%s)" % (item.get("title"), webmap))
    print("  New service: %s" % new_name)

    plan = []
    for L in walk_layers(data.get("operationalLayers")):
        url = L.get("url") or ""
        m = SVC_RE.search(url)
        if not m:
            continue
        old_name, suffix, idx = m.group(1), m.group(2) or "", m.group(3)
        if old_name == new_name:
            print("  already current : %s" % L.get("title"))
            continue
        new_svc_url = url[:m.start()] + "/services/%s%s/FeatureServer" % (new_name, suffix)
        new_url = "%s/%s" % (new_svc_url, idx)
        try:
            svc = rest_json(new_svc_url, token)
            lyr = rest_json(new_url, token)
        except Exception as e:
            print("  ** %s: the new service has no matching layer %s%s/%s (%s) - skipped"
                  % (L.get("title"), new_name, suffix, idx, e))
            continue
        plan.append((L, url, new_url, svc.get("serviceItemId"), lyr.get("name")))

    if not plan:
        print("\n  Nothing to change.\n")
        return 0

    print()
    for L, old, new, iid, lname in plan:
        print("  Layer: %s" % L.get("title"))
        print("     was: %s" % old)
        print("     now: %s   (new layer '%s', item %s)" % (new, lname, iid))

    os.makedirs(os.path.join(HERE, "webmap_backups"), exist_ok=True)
    backup = os.path.join(HERE, "webmap_backups", "webmap_backup_%s_%s.json"
                          % (webmap, datetime.now().strftime("%Y%m%d-%H%M%S")))
    with open(backup, "w") as f:
        json.dump({"item": item, "data": data}, f, indent=2)
    print("\n  Backup written: webmap_backups/%s" % os.path.basename(backup))

    if not write:
        print("\n  Dry run - nothing changed. Run again with --write to apply.\n")
        return 0

    for L, old, new, iid, lname in plan:
        L["url"] = new
        if iid:
            L["itemId"] = iid

    r = requests.post("%s/sharing/rest/content/users/%s/items/%s/update"
                      % (ss.PORTAL, owner, webmap),
                      data={"f": "json", "token": token, "text": json.dumps(data)},
                      timeout=60)
    r.raise_for_status()
    body = r.json()
    if not body.get("success"):
        raise RuntimeError("update failed: %s" % body.get("error", {}).get("message", body))
    print("\n  Web map updated: %d layer(s) now point at %s.\n" % (len(plan), new_name))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, requests.HTTPError) as e:
        print("\n  %s\n" % e)
        sys.exit(1)
