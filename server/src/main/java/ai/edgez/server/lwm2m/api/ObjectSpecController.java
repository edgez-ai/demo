/*******************************************************************************
 * Copyright (c) 2013-2015 Sierra Wireless and others.
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

import java.util.ArrayList;
import java.util.List;

import org.eclipse.leshan.core.model.LwM2mModel;
import org.eclipse.leshan.core.model.ObjectModel;
import org.eclipse.leshan.server.model.LwM2mModelProvider;
import org.eclipse.leshan.server.registration.Registration;
import org.eclipse.leshan.server.registration.RegistrationService;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

import ai.edgez.server.lwm2m.config.LeshanServerComponent;
import ai.edgez.server.lwm2m.model.ObjectModelSerDes;

@RestController
@RequestMapping("/api/objectspecs")
public class ObjectSpecController {

    private final ObjectModelSerDes serializer;
    private final LwM2mModelProvider modelProvider;
    private final RegistrationService registrationService;

    @Autowired
    public ObjectSpecController(LwM2mModelProvider modelProvider, LeshanServerComponent server) {
        this.modelProvider = modelProvider;
        this.serializer = new ObjectModelSerDes();
        this.registrationService = server.getServer().getRegistrationService();
    }

    @GetMapping("/{clientEndpoint}")
    public ResponseEntity<byte[]> getObjectSpec(@PathVariable(name="clientEndpoint") String clientEndpoint) {
        Registration registration = registrationService.getByEndpoint(clientEndpoint);
        if (registration == null) {
            return ResponseEntity.status(HttpStatus.BAD_REQUEST)
                    .body(("no registered client with id '" + clientEndpoint + "'").getBytes());
        }

        LwM2mModel model = modelProvider.getObjectModel(registration);
        List<ObjectModel> objectModels = new ArrayList<>(model.getObjectModels());
        objectModels.sort((o1, o2) -> Integer.compare(o1.id, o2.id));
        byte[] json = serializer.bSerialize(objectModels);

        return ResponseEntity.ok()
                .header("Content-Type", "application/json")
                .body(json);
    }
}
