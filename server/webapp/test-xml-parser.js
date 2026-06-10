#!/usr/bin/env node

/*******************************************************************************
 * Test script for LWM2M XML Parser
 * Verifies that the parser can load and parse XML models correctly
 *******************************************************************************/

import { getAllModels, getModelById } from './api/lwm2m-xml-parser.js';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const MODELS_PATH = join(__dirname, 'models');

console.log('Testing LWM2M XML Parser...\n');
console.log('Models path:', MODELS_PATH);
console.log('-----------------------------------\n');

// Test 1: Load all models
console.log('Test 1: Loading all models...');
try {
  const models = getAllModels(MODELS_PATH);
  console.log(`✓ Successfully loaded ${models.length} models`);
  
  // Show first 5 models
  console.log('\nFirst 5 models:');
  models.slice(0, 5).forEach(model => {
    console.log(`  - ID ${model.id}: ${model.name} (${model.resourcedefs.length} resources)`);
  });
} catch (error) {
  console.error('✗ Failed to load models:', error.message);
}

// Test 2: Get specific model
console.log('\n-----------------------------------');
console.log('Test 2: Getting specific models...\n');

const testIds = [3, 3303, 1];
testIds.forEach(id => {
  try {
    const model = getModelById(id, MODELS_PATH);
    if (model) {
      console.log(`✓ Model ${id} (${model.name}):`);
      console.log(`  - Instance Type: ${model.instancetype}`);
      console.log(`  - Mandatory: ${model.mandatory}`);
      console.log(`  - Version: ${model.version}`);
      console.log(`  - Resources: ${model.resourcedefs.length}`);
      if (model.resourcedefs.length > 0) {
        console.log(`  - First resource: ${model.resourcedefs[0].name} (ID: ${model.resourcedefs[0].id})`);
      }
    } else {
      console.log(`✗ Model ${id} not found`);
    }
  } catch (error) {
    console.error(`✗ Error getting model ${id}:`, error.message);
  }
  console.log('');
});

// Test 3: Check JSON format
console.log('-----------------------------------');
console.log('Test 3: Verifying JSON format...\n');

try {
  const model = getModelById(3303, MODELS_PATH);
  if (model) {
    console.log('Sample JSON output for Temperature sensor (3303):');
    console.log(JSON.stringify(model, null, 2).substring(0, 500) + '...');
    console.log('\n✓ JSON format is valid');
  }
} catch (error) {
  console.error('✗ JSON format error:', error.message);
}

console.log('\n-----------------------------------');
console.log('Tests complete!');
