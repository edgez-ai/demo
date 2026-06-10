/*******************************************************************************
 * Object Specs API Client
 * Provides functions to interact with the objectspecs API
 *******************************************************************************/

/**
 * Fetch object specifications for a given client endpoint
 * @param {string} clientEndpoint - The client endpoint identifier
 * @returns {Promise<Array>} Promise resolving to array of object models
 * @throws {Error} If the request fails or client is not registered
 */
export async function fetchObjectSpecs(clientEndpoint) {
  if (!clientEndpoint) {
    throw new Error('Client endpoint is required');
  }

  const response = await fetch(`/api/objectspecs/${clientEndpoint}`);
  
  if (!response.ok) {
    const errorText = await response.text();
    throw new Error(errorText || `Failed to fetch object specs: ${response.statusText}`);
  }
  
  return response.json();
}

/**
 * Fetch a client registration
 * @param {string} clientEndpoint - The client endpoint identifier
 * @returns {Promise<object|null>} Registration object or null if not found
 */
export async function fetchClientRegistration(clientEndpoint) {
  if (!clientEndpoint) {
    return null;
  }

  const response = await fetch(`/api/clients/${encodeURIComponent(clientEndpoint)}`, {
    headers: { 'Accept': 'application/json' }
  });

  if (!response.ok) {
    return null;
  }

  try {
    return await response.json();
  } catch (err) {
    console.warn('Failed to parse client registration response', err);
    return null;
  }
}

/**
 * Extract object IDs from a registration structure
 * @param {object} registration
 * @returns {Set<number>} Set of object IDs
 */
export function getObjectIdsFromRegistration(registration) {
  const objectIds = new Set();

  if (!registration) {
    return objectIds;
  }

  if (Array.isArray(registration.objects)) {
    registration.objects.forEach(obj => {
      if (typeof obj === 'number') {
        objectIds.add(obj);
      } else if (obj && obj.id !== undefined) {
        const id = parseInt(obj.id, 10);
        if (!Number.isNaN(id)) {
          objectIds.add(id);
        }
      }
    });
  }

  if (Array.isArray(registration.objectLinks)) {
    registration.objectLinks.forEach(link => {
      const url = typeof link === 'string' ? link : link?.url;
      if (typeof url === 'string') {
        const match = url.match(/^\/(\d+)/);
        if (match) {
          objectIds.add(parseInt(match[1], 10));
        }
      }
    });
  }

  if (registration.availableInstances) {
    Object.keys(registration.availableInstances).forEach(key => {
      const id = parseInt(key, 10);
      if (!Number.isNaN(id)) {
        objectIds.add(id);
      }
    });
  }

  if (registration.rootPath && typeof registration.rootPath === 'string') {
    const match = registration.rootPath.match(/^\/(\d+)/);
    if (match) {
      objectIds.add(parseInt(match[1], 10));
    }
  }

  return objectIds;
}

/**
 * Filter models by the object IDs supported in a registration
 * @param {Array} models - All models
 * @param {object} registration - Client registration
 * @returns {Array} Filtered models or original list if no IDs found
 */
export function filterModelsByRegistration(models, registration) {
  if (!registration) {
    return models;
  }

  const supportedIds = getObjectIdsFromRegistration(registration);

  if (supportedIds.size === 0) {
    return models;
  }

  return models.filter(model => supportedIds.has(model.id));
}

/**
 * Get a specific object model by ID from the specs
 * @param {string} clientEndpoint - The client endpoint identifier
 * @param {number} objectId - The object ID to find
 * @returns {Promise<Object|null>} Promise resolving to object model or null if not found
 */
export async function getObjectModelById(clientEndpoint, objectId) {
  const specs = await fetchObjectSpecs(clientEndpoint);
  return specs.find(model => model.id === objectId) || null;
}

/**
 * Search for object models by name (case-insensitive)
 * @param {string} clientEndpoint - The client endpoint identifier
 * @param {string} searchTerm - The search term
 * @returns {Promise<Array>} Promise resolving to array of matching object models
 */
export async function searchObjectModels(clientEndpoint, searchTerm) {
  const specs = await fetchObjectSpecs(clientEndpoint);
  const term = searchTerm.toLowerCase();
  
  return specs.filter(model => 
    model.name.toLowerCase().includes(term) ||
    model.description.toLowerCase().includes(term)
  );
}

export default {
  fetchObjectSpecs,
  fetchClientRegistration,
  getObjectIdsFromRegistration,
  filterModelsByRegistration,
  getObjectModelById,
  searchObjectModels
};
