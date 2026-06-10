package ai.edgez.server.lwm2m.model;

import java.util.EnumSet;

import org.eclipse.leshan.core.request.BindingMode;
import org.eclipse.leshan.core.util.datatype.ULong;

import com.fasterxml.jackson.databind.annotation.JsonDeserialize;
// ...existing code...

public class ServerConfigDto {
    /** Used as link to associate server Object Instance. */
    public int shortId;
    /** Specify the lifetime of the registration in seconds (see Section 5.3 Registration). */
    public int lifetime = 86400;
    /**
     * The default value the LwM2M Client should use for the Minimum Period of an Observation in the absence of this
     * parameter being included in an Observation. If this Resource doesn’t exist, the default value is 0.
     */
    public Integer defaultMinPeriod = 1;
    /**
     * The default value the LwM2M Client should use for the Maximum Period of an Observation in the absence of this
     * parameter being included in an Observation.
     */
    public Integer defaultMaxPeriod = null;
    /**
     * If this Resource is executed, this LwM2M Server Object is disabled for a certain period defined in the
     * Disabled Timeout Resource. After receiving “Execute” operation, LwM2M Client MUST send response of the
     * operation and perform de-registration process, and underlying network connection between the Client and
     * Server MUST be disconnected to disable the LwM2M Server account. After the above process, the LwM2M Client
     * MUST NOT send any message to the Server and ignore all the messages from the LwM2M Server for the period.
     */
    public Integer disableTimeout = null;
    /**
     * If true, the LwM2M Client stores “Notify” operations to the LwM2M Server while the LwM2M Server account is
     * disabled or the LwM2M Client is offline. After the LwM2M Server account is enabled or the LwM2M Client is
     * online, the LwM2M Client reports the stored “Notify” operations to the Server. If false, the LwM2M Client
     * discards all the “Notify” operations or temporarily disables the Observe function while the LwM2M Server is
     * disabled or the LwM2M Client is offline. The default value is true. The maximum number of storing
     * Notifications per Server is up to the implementation.
     */
    public boolean notifIfDisabled = true;
    /**
     * This Resource defines the transport binding configured for the LwM2M Client. If the LwM2M Client supports the
     * binding specified in this Resource, the LwM2M Client MUST use that transport for the Current Binding Mode.
     */
	@JsonDeserialize(using = BindingModeEnumSetDeserializer.class)
	public EnumSet<BindingMode> binding = EnumSet.of(BindingMode.U);

    /**
     * If this resource is defined, it provides a link to the APN connection profile Object Instance (OMNA
     * registered Object ID:11) to be used to communicate with this server.
     * <p>
     * Since Server v1.1
     */
    public Integer apnLink = null;

    /**
     * The LwM2M Client sequences the LwM2M Server registrations in increasing order of this value. If this value is
     * not defined, registration attempts to this server are not impacted by other server registrations.
     */
    public ULong registrationPriority = null;

    /**
     * The delay before registration is attempted for this LwM2M Server based upon the completion of registration of
     * the previous LwM2M Server in the registration order. This is only applied until the first successful
     * registration after a successful bootstrapping sequence.
     */
    public ULong initialDelay = null;

    /**
     * When set to true and registration to this LwM2M server fails, the LwM2M Client blocks registration to other
     * servers in the order. When set to false, the LwM2M Client proceeds with registration to the next server in
     * the order.
     */
    public Boolean registrationFailure = null;

    /**
     * If set to true, this indicates that the LwM2M Client should re-bootstrap when either registration is
     * explicitly rejected by the LwM2M Server or registration is considered as failing as dictated by the other
     * resource settings. If set to false, the LwM2M Client will continue with the registration attempts as dictated
     * by the other resource settings.
     */
    public Boolean bootstrapOnRegistrationFailure = null;

    /**
     * The number of successive communication attempts before which a communication sequence is considered as failed
     */
    public ULong communicationRetryCount = null;

    /**
     * The delay between successive communication attempts in a communication sequence. This value is multiplied by
     * two to the power of the communication retry attempt minus one (2**(retry attempt-1)) to create an exponential
     * back-off.
     */
    public ULong CommunicationRetryTimer = null;

    /**
     * The delay between successive communication sequences. A communication sequence is defined as the exhaustion
     * of the Communication Retry Count and Communication Retry Timer values. A communication sequence can be
     * applied to server registrations or bootstrapping attempts. MAX_VALUE means do not perform another
     * communication sequence.
     */
    public ULong SequenceDelayTimer = null;

    /**
     * The number of successive communication sequences before which a registration attempt is considered as failed.
     */
    public ULong SequenceRetryCount = null;

    /**
     * Using the Trigger Resource a LwM2M Client can indicate whether it is reachable over SMS (value set to 'true')
     * or not (value set to 'false'). The default value (resource not present) is 'false'. When set to 'true' the
     * LwM2M Server MAY, for example, request the LwM2M Client to perform operations, such as the "Update" operation
     * by sending an "Execute" operation on "Registration Update Trigger" Resource via SMS. No SMS response is
     * expected for such a message.
     * <p>
     * Since Server v1.1
     */
    public Boolean trigger = null;

    /**
     * Only a single transport binding SHALL be present. When the LwM2M client supports multiple transports, it MAY
     * use this transport to initiate a connection. This resource can also be used to switch between multiple
     * transports e.g. a non-IP device can switch to UDP transport to perform firmware updates.
     * <p>
     * Since Server v1.1
     */
    public BindingMode preferredTransport = null;
    /**
     * If true or the Resource is not present, the LwM2M Client Send command capability is de-activated. If false,
     * the LwM2M Client Send Command capability is activated. *
     * <p>
     * Since Server v1.1
     */
    public Boolean muteSend = null;
	public int getShortId() {
		return shortId;
	}
	public void setShortId(int shortId) {
		this.shortId = shortId;
	}
	public int getLifetime() {
		return lifetime;
	}
	public void setLifetime(int lifetime) {
		this.lifetime = lifetime;
	}
	public Integer getDefaultMinPeriod() {
		return defaultMinPeriod;
	}
	public void setDefaultMinPeriod(Integer defaultMinPeriod) {
		this.defaultMinPeriod = defaultMinPeriod;
	}
	public Integer getDefaultMaxPeriod() {
		return defaultMaxPeriod;
	}
	public void setDefaultMaxPeriod(Integer defaultMaxPeriod) {
		this.defaultMaxPeriod = defaultMaxPeriod;
	}
	public Integer getDisableTimeout() {
		return disableTimeout;
	}
	public void setDisableTimeout(Integer disableTimeout) {
		this.disableTimeout = disableTimeout;
	}
	public boolean isNotifIfDisabled() {
		return notifIfDisabled;
	}
	public void setNotifIfDisabled(boolean notifIfDisabled) {
		this.notifIfDisabled = notifIfDisabled;
	}
	public EnumSet<BindingMode> getBinding() {
		return binding;
	}
	public void setBinding(EnumSet<BindingMode> binding) {
		this.binding = binding;
	}
	public Integer getApnLink() {
		return apnLink;
	}
	public void setApnLink(Integer apnLink) {
		this.apnLink = apnLink;
	}
	public ULong getRegistrationPriority() {
		return registrationPriority;
	}
	public void setRegistrationPriority(ULong registrationPriority) {
		this.registrationPriority = registrationPriority;
	}
	public ULong getInitialDelay() {
		return initialDelay;
	}
	public void setInitialDelay(ULong initialDelay) {
		this.initialDelay = initialDelay;
	}
	public Boolean getRegistrationFailure() {
		return registrationFailure;
	}
	public void setRegistrationFailure(Boolean registrationFailure) {
		this.registrationFailure = registrationFailure;
	}
	public Boolean getBootstrapOnRegistrationFailure() {
		return bootstrapOnRegistrationFailure;
	}
	public void setBootstrapOnRegistrationFailure(Boolean bootstrapOnRegistrationFailure) {
		this.bootstrapOnRegistrationFailure = bootstrapOnRegistrationFailure;
	}
	public ULong getCommunicationRetryCount() {
		return communicationRetryCount;
	}
	public void setCommunicationRetryCount(ULong communicationRetryCount) {
		this.communicationRetryCount = communicationRetryCount;
	}
	public ULong getCommunicationRetryTimer() {
		return CommunicationRetryTimer;
	}
	public void setCommunicationRetryTimer(ULong communicationRetryTimer) {
		CommunicationRetryTimer = communicationRetryTimer;
	}
	public ULong getSequenceDelayTimer() {
		return SequenceDelayTimer;
	}
	public void setSequenceDelayTimer(ULong sequenceDelayTimer) {
		SequenceDelayTimer = sequenceDelayTimer;
	}
	public ULong getSequenceRetryCount() {
		return SequenceRetryCount;
	}
	public void setSequenceRetryCount(ULong sequenceRetryCount) {
		SequenceRetryCount = sequenceRetryCount;
	}
	public Boolean getTrigger() {
		return trigger;
	}
	public void setTrigger(Boolean trigger) {
		this.trigger = trigger;
	}
	public BindingMode getPreferredTransport() {
		return preferredTransport;
	}
	public void setPreferredTransport(BindingMode preferredTransport) {
		this.preferredTransport = preferredTransport;
	}
	public Boolean getMuteSend() {
		return muteSend;
	}
	public void setMuteSend(Boolean muteSend) {
		this.muteSend = muteSend;
	}
    
    
}
