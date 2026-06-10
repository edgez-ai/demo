package ai.edgez.server.lwm2m.config;

import java.nio.charset.StandardCharsets;

import org.eclipse.californium.core.coap.CoAP;
import org.eclipse.californium.core.coap.Request;
import org.eclipse.californium.core.coap.Response;
import org.eclipse.californium.core.network.Exchange;
import org.eclipse.californium.core.server.MessageDeliverer;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

/**
 * Normalizes /dp SenML JSON payload after Californium blockwise reassembly and
 * right before Leshan decodes it.
 */
public class SendPayloadPostBlockwiseNormalizer implements MessageDeliverer {

    private static final Logger log = LoggerFactory.getLogger(SendPayloadPostBlockwiseNormalizer.class);

    private static final int CONTENT_FORMAT_SENML_JSON = 110;

    private final MessageDeliverer delegate;
    private final ObjectMapper mapper = new ObjectMapper();

    public SendPayloadPostBlockwiseNormalizer(MessageDeliverer delegate) {
        this.delegate = delegate;
    }

    @Override
    public void deliverRequest(Exchange exchange) {
        if (exchange != null) {
            normalizeIfSendDataPush(exchange.getRequest());
        }

        if (delegate != null) {
            delegate.deliverRequest(exchange);
        }
    }

    @Override
    public void deliverResponse(Exchange exchange, Response response) {
        if (delegate != null) {
            delegate.deliverResponse(exchange, response);
        }
    }

    private void normalizeIfSendDataPush(Request request) {
        if (request == null || !isSendDataPush(request)) {
            return;
        }

        byte[] payload = request.getPayload();
        if (payload == null || payload.length == 0) {
            return;
        }

        String json = new String(payload, StandardCharsets.UTF_8);
        try {
            JsonNode root = mapper.readTree(json);
            if (!(root instanceof ArrayNode records)) {
                return;
            }

            boolean changed = false;
            for (JsonNode record : records) {
                if (!(record instanceof ObjectNode obj)) {
                    continue;
                }

                JsonNode vd = obj.get("vd");
                if (vd == null || !vd.isTextual()) {
                    continue;
                }

                String rawVd = vd.asText();
                String normalizedVd = stripTrailingPadding(rawVd);
                if (!normalizedVd.equals(rawVd)) {
                    obj.put("vd", normalizedVd);
                    changed = true;
                }
            }

            if (changed) {
                request.setPayload(mapper.writeValueAsBytes(records));
                log.info("Normalized /dp SenML vd padding post-blockwise before Leshan decode");
            }
        } catch (JsonProcessingException e) {
            log.debug("Unable to parse /dp SenML payload post-blockwise: {}", e.getMessage());
        }
    }

    private static boolean isSendDataPush(Request request) {
        if (request.getCode() != CoAP.Code.POST || request.getOptions() == null) {
            return false;
        }

        String path = request.getOptions().getUriPathString();
        if (path == null) {
            return false;
        }

        String normalizedPath = path.startsWith("/") ? path.substring(1) : path;
        if (!"dp".equals(normalizedPath)) {
            return false;
        }

        return request.getOptions().getContentFormat() == CONTENT_FORMAT_SENML_JSON;
    }

    private static String stripTrailingPadding(String input) {
        int end = input.length();
        while (end > 0 && input.charAt(end - 1) == '=') {
            end--;
        }
        return end == input.length() ? input : input.substring(0, end);
    }
}