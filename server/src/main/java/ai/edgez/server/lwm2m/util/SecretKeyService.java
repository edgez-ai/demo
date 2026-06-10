package ai.edgez.server.lwm2m.util;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;

import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.stereotype.Service;

/**
 * Minimal PSK storage used by {@link ai.edgez.server.lwm2m.config.CustomEditableSecurityStore}.
 * Backed by an embedded H2 file database so keys survive process restarts.
 */
@Service
public class SecretKeyService {
    private static final byte[] DEFAULT_PSK = "defaultpsk".getBytes(StandardCharsets.UTF_8);
    private final JdbcTemplate jdbcTemplate;

    public SecretKeyService(JdbcTemplate jdbcTemplate) {
        this.jdbcTemplate = jdbcTemplate;
    }

    /**
     * Return the PSK associated with the given identity. Falls back to a default value
     * to keep the server running even when no key has been provisioned yet.
     */
    public byte[] getPSK(String identity) {
        if (identity == null || identity.isBlank()) {
            return DEFAULT_PSK;
        }

        byte[] psk = findPSK(identity);
        return psk != null ? psk : DEFAULT_PSK;
    }

    /**
     * Return the exact PSK for an identity, or null if none is provisioned.
     */
    public byte[] findPSK(String identity) {
        if (identity == null || identity.isBlank()) {
            return null;
        }

        return jdbcTemplate.query(
                "SELECT psk FROM psk_keys WHERE identity = ?",
                rs -> {
                    if (!rs.next()) {
                        return null;
                    }
                    return decodeStoredPsk(rs.getBytes("psk"));
                },
                identity);
    }

    /**
     * Store or update the PSK for an identity.
     */
    public void putPSK(String identity, byte[] key) {
        if (identity == null || identity.isBlank() || key == null) {
            return;
        }

        jdbcTemplate.update(
                "MERGE INTO psk_keys (identity, psk, updated_at) VALUES (?, ?, CURRENT_TIMESTAMP)",
                identity,
                key);
    }

    public void deletePSK(String identity) {
        if (identity == null || identity.isBlank()) {
            return;
        }

        jdbcTemplate.update("DELETE FROM psk_keys WHERE identity = ?", identity);
    }

    public void upsertManagedKey(String zone, String endpoint, String identity) {
        if (zone == null || zone.isBlank() || endpoint == null || endpoint.isBlank() || identity == null || identity.isBlank()) {
            return;
        }

        jdbcTemplate.update(
                "MERGE INTO managed_psk (zone, endpoint, identity, updated_at) KEY(zone, endpoint) VALUES (?, ?, ?, CURRENT_TIMESTAMP)",
                zone,
                endpoint,
                identity);
    }

    public String findManagedIdentity(String zone, String endpoint) {
        if (zone == null || zone.isBlank() || endpoint == null || endpoint.isBlank()) {
            return null;
        }

        return jdbcTemplate.query(
                "SELECT identity FROM managed_psk WHERE zone = ? AND endpoint = ?",
                rs -> rs.next() ? rs.getString("identity") : null,
                zone,
                endpoint);
    }

    public String findManagedZone(String endpoint) {
        if (endpoint == null || endpoint.isBlank()) {
            return null;
        }

        return jdbcTemplate.query(
                "SELECT zone FROM managed_psk WHERE endpoint = ? ORDER BY updated_at DESC",
                rs -> rs.next() ? rs.getString("zone") : null,
                endpoint);
    }

    public void deleteManagedKey(String zone, String endpoint) {
        if (zone == null || zone.isBlank() || endpoint == null || endpoint.isBlank()) {
            return;
        }

        jdbcTemplate.update("DELETE FROM managed_psk WHERE zone = ? AND endpoint = ?", zone, endpoint);
    }

    public boolean pskMatches(String identity, String candidatePsk) {
        if (identity == null || identity.isBlank() || candidatePsk == null) {
            return false;
        }

        byte[] current = findPSK(identity);
        if (current == null) {
            return false;
        }

        byte[] candidate = decodePsk(candidatePsk);
        return Arrays.equals(current, candidate);
    }

    /**
     * Accept either hex-encoded PSK (preferred for API transport) or plain text PSK.
     */
    public static byte[] decodePsk(String value) {
        if (value == null) {
            return new byte[0];
        }

        String normalized = value.trim();
        if (isEvenLengthHex(normalized)) {
            int len = normalized.length();
            byte[] out = new byte[len / 2];
            for (int i = 0; i < len; i += 2) {
                int hi = Character.digit(normalized.charAt(i), 16);
                int lo = Character.digit(normalized.charAt(i + 1), 16);
                out[i / 2] = (byte) ((hi << 4) | lo);
            }
            return out;
        }

        return normalized.getBytes(StandardCharsets.UTF_8);
    }

    private static boolean isEvenLengthHex(String value) {
        if (value == null || value.isEmpty() || (value.length() % 2) != 0) {
            return false;
        }

        for (int i = 0; i < value.length(); i++) {
            if (Character.digit(value.charAt(i), 16) < 0) {
                return false;
            }
        }
        return true;
    }

    private static byte[] decodeStoredPsk(byte[] stored) {
        if (stored == null || stored.length == 0) {
            return stored;
        }

        // If DB stores PSK as UTF-8 hex text, decode to raw bytes for DTLS usage.
        String asText = new String(stored, StandardCharsets.UTF_8).trim();
        if (!asText.isEmpty() && asText.length() == stored.length && isEvenLengthHex(asText)) {
            return decodePsk(asText);
        }

        // Already raw/binary (or plain text non-hex) - keep as stored.
        return stored;
    }
}
