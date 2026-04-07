# WhatsApp Webhook Tunnel (cloudflared)

To receive real-time messages from Meta, your local Luotsi node must be reachable from the internet. We use **Cloudflare Tunnel (`cloudflared`)** for this.

## 1. Install cloudflared
On Linux:
```bash
curl -L --output cloudflared.deb https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64.deb
sudo dpkg -i cloudflared.deb
```

## 2. Start the tunnel
Run this in a separate terminal to expose your gateway node (port 8090):
```bash
cloudflared tunnel --url http://localhost:8090
```
Look for a line like:
`+  Your quick tunnel has been created! Visit: https://random-words.trycloudflare.com`

## 3. Configure Meta Dashboard
1. Go to [developers.facebook.com/apps](https://developers.facebook.com/apps) → your app.
2. WhatsApp → **Configuration**.
3. Click **Edit** next to "Webhook".
4. **Callback URL** (example): `https://your-random-words.trycloudflare.com/webhook`
5. **Verify Token** (example): `luotsi_verify_123` (matching your `.env`)
6. Click **Verify and Save**.

## 4. Subscribe to Messages
1. In the same WhatsApp Configuration page, find the **Webhook fields** table.
2. Click **Manage**.
3. Subscribe to `messages`.

## 5. Add Test Numbers & Access Token
1. In Meta Dashboard, go to **WhatsApp → API Setup**.
2. Under **Step 1: Select phone numbers**, find the "To" field.
3. You **must** add your personal phone number to the allowed list (and verify it) for messages to be accepted in development mode.
4. If you haven't already, **generate an Access Token** (permanent or temporary) from the **App Dashboard** or **System Users** page.
> [!NOTE]
> Temporary test tokens are only valid for few hours. For long-term testing, create a System User in your Meta Business Suite and generate a permanent token.
5. Add this token to your node's [`.env`](file:///home/andy/code/luotsi/playground/wa-gateway/.env) file: `ACCESS_TOKEN=your_token_here`.

---

**Now, when you send a message to your WhatsApp Business number, it will hit your local `wa_gateway.py` node!**
