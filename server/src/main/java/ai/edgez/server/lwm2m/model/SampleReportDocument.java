package ai.edgez.server.lwm2m.model;

import java.time.Instant;
import java.util.Objects;

import org.springframework.data.annotation.Id;
import org.springframework.data.mongodb.core.mapping.Document;
import org.springframework.data.mongodb.core.mapping.Field;

@Document(collection = "mqtt_like_data")
public class SampleReportDocument {

    @Id
    private String id;
    @Field("timestamp")
    private Instant timestamp;
    @Field("peerid")
    private String peerId;
    @Field("serial")
    private String serial;
    @Field("zone")
    private String zone;
    @Field("lwm2m_object")
    private int object;
    @Field("instance")
    private int instance;
    @Field("resource")
    private int resource;
    @Field("type")
    private String type;
    @Field("value")
    private Object value;

    public SampleReportDocument() {
        // default constructor for Mongo mapping
    }

    public SampleReportDocument(String id,
                                Instant timestamp,
                                String peerId,
                                String serial,
                                String zone,
                                int object,
                                int instance,
                                int resource,
                                String type,
                                Object value) {
        this.id = id;
        this.timestamp = Objects.requireNonNull(timestamp, "timestamp");
        this.peerId = peerId;
        this.serial = serial;
        this.zone = zone;
        this.object = object;
        this.instance = instance;
        this.resource = resource;
        this.type = type;
        this.value = value;
    }

    public String getId() {
        return id;
    }

    public void setId(String id) {
        this.id = id;
    }

    public Instant getTimestamp() {
        return timestamp;
    }

    public void setTimestamp(Instant timestamp) {
        this.timestamp = timestamp;
    }

    public String getPeerId() {
        return peerId;
    }

    public void setPeerId(String peerId) {
        this.peerId = peerId;
    }

    public String getSerial() {
        return serial;
    }

    public void setSerial(String serial) {
        this.serial = serial;
    }

    public String getZone() {
        return zone;
    }

    public void setZone(String zone) {
        this.zone = zone;
    }

    public int getObject() {
        return object;
    }

    public void setObject(int object) {
        this.object = object;
    }

    public int getInstance() {
        return instance;
    }

    public void setInstance(int instance) {
        this.instance = instance;
    }

    public int getResource() {
        return resource;
    }

    public void setResource(int resource) {
        this.resource = resource;
    }

    public String getType() {
        return type;
    }

    public void setType(String type) {
        this.type = type;
    }

    public Object getValue() {
        return value;
    }

    public void setValue(Object value) {
        this.value = value;
    }
}
