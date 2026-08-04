# Cluster Integration with Secure DESFire ESP32 Tap

The `tagreader` ESP32 wallplate performs physical AES-128 cryptographic challenge-responses against DESFire EV2 NFC cards. Once authenticated, the hardware acts as an identity assertion bridge to our local Authentik Identity Provider (IdP) and downstream orchestration tools like the `llama-cluster-upbringing-script`.

## Architecture Flow

1.  **Physical Tap**: User taps DESFire card on the ESP32 reader.
2.  **Hardware APDU Auth**: The ESP32 performs a 3-pass mutual AES-128 authentication.
3.  **MQTT Publish**: Upon cryptographic success, the ESP32 publishes an event to the local MQTT broker.
4.  **Event Ingestion**: A daemon (or script like `llama-cluster-upbringing-script`) subscribed to MQTT consumes the event.
5.  **Authentik Authorization**: The daemon calls Authentik (via M2M Service Account or Webhook) to map the `user_id` to groups and scopes.
6.  **Infrastructure Orchestration**: The daemon provisions resources (Home Assistant scenes, GPU cluster nodes, SSH certificates) based on the Authentik authorization.

## MQTT Event Payload Schema

The ESP32 publishes to the `tagreader/auth/success` topic with the following JSON schema:

```json
{
  "user_id": "lawrence",
  "reader_id": "front_door",
  "timestamp": 1785860545
}
```

*   `user_id`: The internal user identifier mapped from the card authentication.
*   `reader_id`: The physical location or identifier of the reader plate.
*   `timestamp`: Unix timestamp of the authentication event (requires ESP32 `time` component sync).

## Integration with Authentik

A receiving script (e.g. part of `llama-cluster-upbringing-script`) should perform the following:

1.  **Listen to MQTT**: Subscribe to `tagreader/auth/success`.
2.  **M2M Token Exchange**: Use a configured Client ID/Secret to obtain an OAuth2 token from Authentik's `/application/o/token/` endpoint.
3.  **User Verification**: Use the token to query the Authentik API for the `user_id`'s current group memberships and active status.
    ```bash
    curl -H "Authorization: Bearer <TOKEN>" https://authentik.local/api/v3/core/users/?username=lawrence
    ```

## Triggering Cluster Hot-Loading

Once the identity is confirmed and authorized by Authentik, the upbringing script can execute context-specific tasks:

*   **Compute Nodes**: Use Proxmox/Kubernetes APIs to spin up or allocate GPU nodes assigned to the user.
*   **Vector Mounts**: Mount the user's specific vectorized memory stores or fine-tuned adapters into the active container environment.
*   **SSH Access**: Use a local PKI (like HashiCorp Vault or Smallstep CA) to generate an ephemeral SSH certificate matching the user's IAM role, injecting it into their session.
