/*******************************************************************************
 * Copyright (c) 2021 Sierra Wireless and others.
 * 
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v2.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 * 
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v20.html
 * and the Eclipse Distribution License is available at
 *    http://www.eclipse.org/org/documents/edl-v10.html.
 *******************************************************************************/
import { Vuetify3Dialog } from 'vuetify3-dialog'

export default {
  install(app) {
    app.use(Vuetify3Dialog, {
      defaults: {
        dialog: {
          persistent: false
        }
      }
    })
    
    // Add a custom prompt method since vuetify3-dialog doesn't provide one
    // We'll use the native window.prompt as a fallback
    if (app.config.globalProperties.$dialog) {
      app.config.globalProperties.$dialog.prompt = async function(message, defaultValue = '') {
        return window.prompt(message, defaultValue);
      }
    }
  }
};