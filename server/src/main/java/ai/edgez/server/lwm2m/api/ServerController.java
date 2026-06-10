/*******************************************************************************
 * Copyright (c) 2025 Sierra Wireless and others.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v20.html
 * and the Eclipse Distribution License is available at
 *    http://www.eclipse.org/org/documents/edl-v10.html.
 *
 * Contributors:
 *     Sierra Wireless - initial API and implementation
 *******************************************************************************/
package ai.edgez.server.lwm2m.api;

import java.util.List;

import org.eclipse.leshan.server.endpoint.LwM2mServerEndpoint;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.JsonNodeFactory;
import com.fasterxml.jackson.databind.node.ObjectNode;

import ai.edgez.server.lwm2m.config.LeshanServerComponent;


@RestController
@RequestMapping("/api/server")
public class ServerController {

    private final List<? extends LwM2mServerEndpoint> endpoints;

    public ServerController(LeshanServerComponent server) {
        this.endpoints = server.getServer().getEndpoints();
    }

    @GetMapping("/security")
    public ObjectNode getServerSecurityInfo() {
        ObjectNode security = JsonNodeFactory.instance.objectNode();

        return security;
    }

    @GetMapping("/endpoint")
    public ArrayNode getEndpoints() {
        ArrayNode endpointsArrayNode = JsonNodeFactory.instance.arrayNode();
        for (LwM2mServerEndpoint endpoint : endpoints) {
            ObjectNode ep = JsonNodeFactory.instance.objectNode();
            ObjectNode uri = JsonNodeFactory.instance.objectNode();
            ep.set("uri", uri);
            uri.put("full", endpoint.getURI().toString());
            uri.put("scheme", endpoint.getURI().getScheme());
            uri.put("host", endpoint.getURI().getHost());
            uri.put("port", endpoint.getURI().getPort());
            ep.put("description", endpoint.getDescription());
            endpointsArrayNode.add(ep);
        }
        return endpointsArrayNode;
    }
}
