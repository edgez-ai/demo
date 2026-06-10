package ai.edgez.server.lwm2m.model;

import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.databind.DeserializationContext;
import com.fasterxml.jackson.databind.JsonDeserializer;
import com.fasterxml.jackson.databind.JsonNode;
import org.eclipse.leshan.core.request.BindingMode;
import java.io.IOException;
import java.util.EnumSet;
import java.util.Iterator;

public class BindingModeEnumSetDeserializer extends JsonDeserializer<EnumSet<BindingMode>> {
    @Override
    public EnumSet<BindingMode> deserialize(JsonParser p, DeserializationContext ctxt) throws IOException {
        JsonNode node = p.getCodec().readTree(p);
        EnumSet<BindingMode> result = EnumSet.noneOf(BindingMode.class);
        if (node.isArray()) {
            for (JsonNode item : node) {
                result.add(BindingMode.valueOf(item.asText()));
            }
        } else if (node.isTextual()) {
            String text = node.asText();
            for (String part : text.split(",")) {
                result.add(BindingMode.valueOf(part.trim()));
            }
        }
        return result;
    }
}
