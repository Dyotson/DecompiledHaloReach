"""Reach Live title servers ("LSP"): the HTTP services Bungie ran for Halo: Reach.

reach_live_server.py calls handle() for every HTTP request on --http-port that isn't the
status page. See docs/online_plan.md section 5.
"""


def handle(server, method, path, headers, body, peer):
    """Returns (HTTP status, content type, body bytes)."""
    return 404, "text/plain", b"not found"
