/*******************************************************************************
 * API for Object Specifications
 * Provides /api/objectspecs endpoint using real LWM2M XML models
 * Filters models based on client's registered object links
 *******************************************************************************/

import { getAllModels } from './lwm2m-xml-parser.js';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Path to the models directory (linked from resources/models)
const MODELS_PATH = join(__dirname, '..', 'models');

let cachedModels = null;

/**
 * Load models from XML files
 * @returns {Array} Array of object models
 */
function loadModels() {
  if (!cachedModels) {
    try {
      cachedModels = getAllModels(MODELS_PATH);
      console.log(`Loaded ${cachedModels.length} LWM2M object models from XML files`);
    } catch (error) {
      console.error('Failed to load models from XML, using empty array:', error);
      cachedModels = [];
    }
  }
  return cachedModels;
}

/**
 * Handle objectspecs API requests
 * @param {string} clientEndpoint - The client endpoint to get specs for
 * @returns {Promise<object>} Response object with status and data
 */
export async function getObjectSpecs(clientEndpoint) {
  if (!clientEndpoint || clientEndpoint.trim() === '') {
    return {
      status: 400,
      data: `no registered client with id '${clientEndpoint}'`
    };
  }

  // Load all available models from XML files
  const allModels = loadModels();
  
  if (allModels.length === 0) {
    return {
      status: 500,
      data: 'Failed to load object models'
    };
  }
  
  // Return full model set; client handles filtering
  const sortedModels = [...allModels].sort((a, b) => a.id - b.id);
  
  return {
    status: 200,
    data: sortedModels,
    headers: {
      'Content-Type': 'application/json'
    }
  };
}

/**
 * Vite middleware handler for /api/objectspecs endpoint
 */
export async function objectSpecsMiddleware(req, res, next) {
  const urlPattern = /^\/api\/objectspecs\/([^\/]+)$/;
  const match = req.url.match(urlPattern);
  
  if (match) {
    const clientEndpoint = match[1];
    
    try {
      const response = await getObjectSpecs(clientEndpoint);
      
      res.statusCode = response.status;
      
      if (response.headers) {
        Object.entries(response.headers).forEach(([key, value]) => {
          res.setHeader(key, value);
        });
      }
      
      if (typeof response.data === 'string') {
        res.end(response.data);
      } else {
        res.setHeader('Content-Type', 'application/json');
        res.end(JSON.stringify(response.data));
      }
    } catch (error) {
      console.error('Error in objectSpecsMiddleware:', error);
      res.statusCode = 500;
      res.end('Internal server error');
    }
    
    return;
  }
  
  next();
}
