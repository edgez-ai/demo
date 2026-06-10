package ai.edgez.server.lwm2m.util;

import java.time.Instant;
import java.util.Locale;

import org.bson.Document;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.data.mongodb.core.MongoTemplate;
import org.springframework.stereotype.Service;

@Service
@ConditionalOnProperty(prefix = "lwm2m.report.mongodb", name = "enabled", havingValue = "true")
public class SampleReportMongoService {

    private static final Logger LOG = LoggerFactory.getLogger(SampleReportMongoService.class);

    private final MongoTemplate mongoTemplate;
    private final String collectionName;

    public SampleReportMongoService(MongoTemplate mongoTemplate,
                                    @Value("${lwm2m.report.mongodb.collection:mqtt_like_data}") String collectionName) {
        this.mongoTemplate = mongoTemplate;
        this.collectionName = collectionName;
    }

    public void saveResourceReport(String peerId,
                                   String serial,
                                   String zone,
                                   int object,
                                   int instance,
                                   int resource,
                                   String type,
                                   Object value,
                                   Instant timestamp) {
        Instant ts = timestamp == null ? Instant.now() : timestamp;
        String resolvedType = (value instanceof Number)
            ? (type == null ? "unknown" : type.trim().toLowerCase(Locale.ROOT))
            : "b64";
                    Document doc = new Document();
                    doc.put("_id", String.valueOf(System.nanoTime()));
                    doc.put("timestamp", ts);
                    doc.put("peerid", peerId);
                    doc.put("serial", serial);
                    doc.put("zone", zone == null ? "" : zone);
                    doc.put("lwm2m_object", object);
                    doc.put("instance", instance);
                    doc.put("resource", resource);
                    doc.put("type", resolvedType);
                    doc.put("value", value);

            LOG.info("Mongo save attempt collection={} peerId={} serial={} path={}/{}/{} type={}",
                collectionName,
                peerId,
                serial,
                object,
                instance,
                resource,
                resolvedType);

        mongoTemplate.getCollection(collectionName).insertOne(doc);
            LOG.info("Saved sample report record to MongoDB collection={} peerId={} serial={} path={}/{}/{}",
                collectionName,
                peerId,
                serial,
                object,
                instance,
                resource);
    }
}
