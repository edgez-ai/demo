package ai.edgez.server.lwm2m.api;

import java.util.Locale;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import jakarta.servlet.http.HttpServletRequest;

final class ZoneResolver {

    private static final Logger log = LoggerFactory.getLogger(ZoneResolver.class);

    private ZoneResolver() {
    }

    static String resolveZone(HttpServletRequest request) {
        String explicit = firstNonBlank(
                request.getHeader("X-Edge-Zone"),
                request.getHeader("X-Zone"),
                request.getParameter("zone"));
        if (!isBlank(explicit)) {
            String resolved = sanitizeZone(explicit);
            log.debug("zone resolve source=explicit resolved={} method={} uri={} host={} xEdgeZone={} xZone={} queryZone={} remote={}",
                resolved,
                request.getMethod(),
                request.getRequestURI(),
                request.getHeader("Host"),
                request.getHeader("X-Edge-Zone"),
                request.getHeader("X-Zone"),
                request.getParameter("zone"),
                request.getRemoteAddr());
            return resolved;
        }

        String forwardedHost = extractForwardedHost(request.getHeader("Forwarded"));
        String[] hostCandidates = {
            forwardedHost,
            request.getHeader("X-Forwarded-Host"),
            request.getHeader("X-Original-Host"),
            request.getHeader("Host"),
            request.getServerName()
        };
        for (String hostCandidate : hostCandidates) {
            String zone = zoneFromHost(hostCandidate);
            if (!isBlank(zone)) {
                log.debug(
                        "zone resolve source=host resolved={} candidate={} method={} uri={} forwarded={} xForwardedHost={} xOriginalHost={} host={} serverName={} remote={}",
                        zone,
                        hostCandidate,
                        request.getMethod(),
                        request.getRequestURI(),
                        request.getHeader("Forwarded"),
                        request.getHeader("X-Forwarded-Host"),
                        request.getHeader("X-Original-Host"),
                        request.getHeader("Host"),
                        request.getServerName(),
                        request.getRemoteAddr());
                return zone;
            }
        }

        log.debug(
            "zone resolve source=unresolved resolved=null method={} uri={} forwarded={} xForwardedHost={} xOriginalHost={} host={} serverName={} remote={}",
            request.getMethod(),
            request.getRequestURI(),
            request.getHeader("Forwarded"),
            request.getHeader("X-Forwarded-Host"),
            request.getHeader("X-Original-Host"),
            request.getHeader("Host"),
            request.getServerName(),
            request.getRemoteAddr());
        return null;
    }

    private static String zoneFromHost(String rawHost) {
        if (isBlank(rawHost)) {
            return null;
        }

        String host = stripPortAndNormalize(firstHost(rawHost));
        if (isBlank(host) || isLocalOrIp(host)) {
            return null;
        }

        String[] labels = host.split("\\.");
        if (labels.length >= 3) {
            return sanitizeZone(labels[1]);
        }
        if (labels.length == 2) {
            return sanitizeZone(labels[1]);
        }
        return null;
    }

    private static String extractForwardedHost(String forwardedHeader) {
        if (isBlank(forwardedHeader)) {
            return null;
        }

        String[] segments = forwardedHeader.split(";");
        for (String segment : segments) {
            String trimmed = segment.trim();
            if (trimmed.regionMatches(true, 0, "host=", 0, 5)) {
                return trimmed.substring(5).trim();
            }
        }
        return null;
    }

    private static String firstHost(String hostHeader) {
        int commaIndex = hostHeader.indexOf(',');
        return commaIndex >= 0 ? hostHeader.substring(0, commaIndex).trim() : hostHeader.trim();
    }

    private static String stripPortAndNormalize(String host) {
        String out = host;
        if (out.startsWith("\"") && out.endsWith("\"") && out.length() > 1) {
            out = out.substring(1, out.length() - 1);
        }

        // IPv6 in host header is commonly wrapped in brackets.
        if (out.startsWith("[") && out.contains("]")) {
            int end = out.indexOf(']');
            out = out.substring(1, end);
            return out.toLowerCase(Locale.ROOT);
        }

        int colon = out.indexOf(':');
        if (colon >= 0) {
            out = out.substring(0, colon);
        }

        return out.toLowerCase(Locale.ROOT);
    }

    private static String sanitizeZone(String zone) {
        if (isBlank(zone)) {
            return null;
        }

        String sanitized = zone.trim().toLowerCase(Locale.ROOT);
        return sanitized.isBlank() ? null : sanitized;
    }

    private static boolean isLocalOrIp(String host) {
        if ("localhost".equalsIgnoreCase(host)) {
            return true;
        }
        if (host.matches("\\d+\\.\\d+\\.\\d+\\.\\d+")) {
            return true;
        }
        return host.contains(":");
    }

    private static boolean isBlank(String value) {
        return value == null || value.isBlank();
    }

    private static String firstNonBlank(String... values) {
        for (String value : values) {
            if (!isBlank(value)) {
                return value;
            }
        }
        return null;
    }
}