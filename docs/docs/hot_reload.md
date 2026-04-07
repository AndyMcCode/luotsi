# Hot Reloading

Luotsi supports reloading its configuration without restarting the entire process. This allows you to add, remove, or modify nodes and routes on the fly.

## Triggering a Reload

To trigger a reload, send the `SIGHUP` signal to the running `luotsi` process:

```bash
kill -HUP <PID>
```

## Behavior

When specific changes are made to `luotsi.config.yaml`:

| Change Type | Behavior |
| :--- | :--- |
| **Add Node** | The new node is started immediately. |
| **Remove Node** | The node is stopped (child process killed / socket closed). |
| **Modify Runtime** | If `command`, `args`, or `adapter` changes, the node is **restarted**. |
| **Modify Routes** | Routing table is updated instantly. The node process is **NOT** restarted. |

## Automation

To automatically reload on file save, you can use a file watcher like `entr`:

```bash
ls luotsi.config.yaml | entr -p kill -HUP $(pgrep luotsi)
```

