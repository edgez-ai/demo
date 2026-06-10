package ai.edgez.server.lwm2m.util;

import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.databind.DeserializationContext;
import com.fasterxml.jackson.databind.JsonDeserializer;
import com.fasterxml.jackson.databind.JsonNode;
import org.eclipse.leshan.core.request.ContentFormat;

import java.io.IOException;

public class ContentFormatDeserializer extends JsonDeserializer<ContentFormat> {
    @Override
    public ContentFormat deserialize(JsonParser p, DeserializationContext ctxt) throws IOException {
        JsonNode node = p.getCodec().readTree(p);
        String name = node.has("name") ? node.get("name").asText() : null;
        String mediaType = node.has("mediaType") ? node.get("mediaType").asText() : null;
        int code = node.has("code") ? node.get("code").asInt() : 0;
        return new ContentFormat(name, mediaType, code);
    }
}