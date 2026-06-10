/*******************************************************************************
 * LWM2M XML Model Parser
 * Parses LWM2M object models from XML files (OMA format)
 *******************************************************************************/

import { readFileSync, readdirSync } from 'fs';
import { join, basename } from 'path';
import { fileURLToPath } from 'url';
import { dirname } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Cache for parsed models
let modelsCache = null;

/**
 * Parse XML text to extract LWM2M object model
 * @param {string} xmlText - The XML content
 * @returns {object|null} Parsed object model or null if invalid
 */
function parseObjectModelXML(xmlText) {
  try {
    // Extract object ID from ObjectID tag
    const objectIdMatch = xmlText.match(/<ObjectID>(\d+)<\/ObjectID>/);
    if (!objectIdMatch) return null;
    const id = parseInt(objectIdMatch[1]);

    // Extract name
    const nameMatch = xmlText.match(/<Name[^>]*>(.*?)<\/Name>/);
    const name = nameMatch ? nameMatch[1].trim() : '';

    // Extract description
    const descMatch = xmlText.match(/<Description1[^>]*>([\s\S]*?)<\/Description1>/);
    const description = descMatch ? descMatch[1].replace(/\s+/g, ' ').trim() : '';

    // Extract description2
    const desc2Match = xmlText.match(/<Description2[^>]*>([\s\S]*?)<\/Description2>/);
    const description2 = desc2Match ? desc2Match[1].replace(/\s+/g, ' ').trim() : '';

    // Extract URN
    const urnMatch = xmlText.match(/<ObjectURN>(.*?)<\/ObjectURN>/);
    const urn = urnMatch ? urnMatch[1].trim() : '';

    // Extract version
    const versionMatch = xmlText.match(/<ObjectVersion>(.*?)<\/ObjectVersion>/);
    const version = versionMatch ? versionMatch[1].trim() : '1.0';

    // Extract LWM2M version
    const lwm2mVersionMatch = xmlText.match(/<LWM2MVersion>(.*?)<\/LWM2MVersion>/);
    const lwm2mversion = lwm2mVersionMatch ? lwm2mVersionMatch[1].trim() : '1.0';

    // Extract MultipleInstances
    const multipleMatch = xmlText.match(/<MultipleInstances>(.*?)<\/MultipleInstances>/);
    const instancetype = multipleMatch && multipleMatch[1].trim() === 'Multiple' ? 'multiple' : 'single';

    // Extract Mandatory
    const mandatoryMatch = xmlText.match(/<Mandatory>(.*?)<\/Mandatory>/);
    const mandatory = mandatoryMatch && mandatoryMatch[1].trim() === 'Mandatory';

    // Extract resources
    const resourcedefs = [];
    const itemRegex = /<Item ID="(\d+)">([\s\S]*?)<\/Item>/g;
    let itemMatch;

    while ((itemMatch = itemRegex.exec(xmlText)) !== null) {
      const resourceId = parseInt(itemMatch[1]);
      const itemContent = itemMatch[2];

      // Parse resource fields
      const resNameMatch = itemContent.match(/<Name[^>]*>(.*?)<\/Name>/);
      const resName = resNameMatch ? resNameMatch[1].trim() : '';

      const resOpsMatch = itemContent.match(/<Operations>(.*?)<\/Operations>/);
      const operations = resOpsMatch ? resOpsMatch[1].trim() : '';

      const resMultiMatch = itemContent.match(/<MultipleInstances>(.*?)<\/MultipleInstances>/);
      const resInstancetype = resMultiMatch && resMultiMatch[1].trim() === 'Multiple' ? 'multiple' : 'single';

      const resMandMatch = itemContent.match(/<Mandatory>(.*?)<\/Mandatory>/);
      const resMandatory = resMandMatch && resMandMatch[1].trim() === 'Mandatory';

      const resTypeMatch = itemContent.match(/<Type>(.*?)<\/Type>/);
      const type = resTypeMatch ? resTypeMatch[1].trim().toLowerCase() : 'string';

      const resRangeMatch = itemContent.match(/<RangeEnumeration>(.*?)<\/RangeEnumeration>/);
      const range = resRangeMatch ? resRangeMatch[1].trim() : '';

      const resUnitsMatch = itemContent.match(/<Units>(.*?)<\/Units>/);
      const units = resUnitsMatch ? resUnitsMatch[1].trim() : '';

      const resDescMatch = itemContent.match(/<Description>([\s\S]*?)<\/Description>/);
      const resDescription = resDescMatch ? resDescMatch[1].replace(/\s+/g, ' ').trim() : '';

      resourcedefs.push({
        id: resourceId,
        name: resName,
        operations: operations,
        instancetype: resInstancetype,
        mandatory: resMandatory,
        type: type,
        range: range,
        units: units,
        description: resDescription
      });
    }

    // Sort resources by ID
    resourcedefs.sort((a, b) => a.id - b.id);

    return {
      id,
      name,
      instancetype,
      mandatory,
      urn,
      version,
      lwm2mversion,
      description,
      description2,
      resourcedefs
    };
  } catch (error) {
    console.error('Error parsing XML:', error);
    return null;
  }
}

/**
 * Compare two version strings (e.g., "1.0", "1.1")
 * @param {string} v1 - First version
 * @param {string} v2 - Second version
 * @returns {number} -1 if v1 < v2, 0 if equal, 1 if v1 > v2
 */
function compareVersions(v1, v2) {
  if (!v1) return -1;
  if (!v2) return 1;
  
  const parts1 = v1.split('.').map(p => parseInt(p) || 0);
  const parts2 = v2.split('.').map(p => parseInt(p) || 0);
  
  for (let i = 0; i < Math.max(parts1.length, parts2.length); i++) {
    const p1 = parts1[i] || 0;
    const p2 = parts2[i] || 0;
    if (p1 < p2) return -1;
    if (p1 > p2) return 1;
  }
  return 0;
}

/**
 * Load all XML models from the models directory
 * @param {string} modelsPath - Path to the models directory
 * @returns {Array} Array of parsed object models
 */
export function loadModelsFromDirectory(modelsPath) {
  if (!modelsPath) {
    // Default path - assuming models are linked/copied to webapp
    modelsPath = join(__dirname, '..', '..', 'models');
  }

  const modelsMap = new Map(); // Use Map to deduplicate by ID

  try {
    const files = readdirSync(modelsPath);
    
    for (const file of files) {
      if (file.endsWith('.xml')) {
        const filePath = join(modelsPath, file);
        const xmlContent = readFileSync(filePath, 'utf-8');
        const model = parseObjectModelXML(xmlContent);
        
        if (model) {
          const existingModel = modelsMap.get(model.id);
          
          if (!existingModel) {
            // First model with this ID, add it
            modelsMap.set(model.id, model);
          } else {
            // Duplicate ID - keep the one with higher version or newer LWM2M version
            const versionComparison = compareVersions(model.version, existingModel.version);
            const lwm2mComparison = compareVersions(model.lwm2mversion, existingModel.lwm2mversion);
            
            // Prefer higher version, or higher LWM2M version if versions are equal
            if (versionComparison > 0 || (versionComparison === 0 && lwm2mComparison > 0)) {
              modelsMap.set(model.id, model);
              console.log(`Replaced model ${model.id} with newer version ${model.version} (LWM2M ${model.lwm2mversion})`);
            }
          }
        }
      }
    }

    // Convert map to array and sort by ID
    const models = Array.from(modelsMap.values());
    models.sort((a, b) => a.id - b.id);
    
    console.log(`Loaded ${models.length} unique models (deduplicated from ${modelsMap.size} total)`);
    
    return models;
  } catch (error) {
    console.error('Error loading models from directory:', error);
    return [];
  }
}

/**
 * Get all models (cached)
 * @param {string} modelsPath - Optional path to models directory
 * @returns {Array} Array of object models
 */
export function getAllModels(modelsPath) {
  if (!modelsCache) {
    modelsCache = loadModelsFromDirectory(modelsPath);
  }
  return modelsCache;
}

/**
 * Get a specific model by ID
 * @param {number} objectId - The object ID
 * @param {string} modelsPath - Optional path to models directory
 * @returns {object|null} The object model or null
 */
export function getModelById(objectId, modelsPath) {
  const models = getAllModels(modelsPath);
  return models.find(m => m.id === objectId) || null;
}

/**
 * Clear the models cache (useful for testing or reloading)
 */
export function clearCache() {
  modelsCache = null;
}

export default {
  parseObjectModelXML,
  loadModelsFromDirectory,
  getAllModels,
  getModelById,
  clearCache
};
