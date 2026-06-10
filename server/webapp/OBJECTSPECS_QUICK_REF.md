# Quick Reference: Object Specs API

## Setup (One-time)

```bash
cd webapp
npm install  # Automatically links models directory
```

## Usage in Code

### Fetch Object Specs

```javascript
import { fetchObjectSpecs } from '@/js/objectspecs-api.js';

const specs = await fetchObjectSpecs('client-endpoint-id');
```

### Get Specific Model

```javascript
import { getObjectModelById } from '@/js/objectspecs-api.js';

const tempModel = await getObjectModelById('client-id', 3303);
console.log(tempModel.name); // "Temperature"
```

### Search Models

```javascript
import { searchObjectModels } from '@/js/objectspecs-api.js';

const results = await searchObjectModels('client-id', 'temperature');
```

### Use in Vue Component

```vue
<script setup>
import { ref, onMounted } from 'vue';
import { fetchObjectSpecs } from '@/js/objectspecs-api.js';

const models = ref([]);

onMounted(async () => {
  models.value = await fetchObjectSpecs('12345-ABC');
});
</script>
```

## Direct API Access

```bash
# Get all models for a client
curl http://localhost:8088/api/objectspecs/12345-7C2C67FFFE53
```

## Testing Commands

```bash
# Test XML parser
npm run test-parser

# Start dev server
npm run serve

# Recreate models symlink
npm run setup-models
```

## File Structure

```
webapp/
├── api/
│   ├── lwm2m-xml-parser.js    # XML → JSON parser
│   └── objectspecs.js          # API endpoint handler
├── src/
│   ├── js/
│   │   └── objectspecs-api.js  # Client utilities
│   └── components/
│       └── ObjectSpecsBrowser.vue  # Example UI
├── models/                     # → symlink to ../src/main/resources/models
├── setup-models-link.sh       # Setup script
├── test-xml-parser.js         # Parser test
└── vite.config.js             # Dev server config

../src/main/resources/models/  # 341 XML files (source)
```

## Common Patterns

### Loading Models on Component Mount

```vue
<script setup>
import { ref, onMounted } from 'vue';
import { fetchObjectSpecs } from '@/js/objectspecs-api.js';

const models = ref([]);
const loading = ref(false);
const error = ref(null);

onMounted(async () => {
  loading.value = true;
  try {
    models.value = await fetchObjectSpecs(clientEndpoint.value);
  } catch (err) {
    error.value = err.message;
  } finally {
    loading.value = false;
  }
});
</script>
```

### Filter Models by Type

```javascript
const specs = await fetchObjectSpecs('client-id');

// Get only mandatory models
const mandatory = specs.filter(m => m.mandatory);

// Get only multiple instance objects
const multiple = specs.filter(m => m.instancetype === 'multiple');

// Get IPSO objects (3xxx range)
const ipso = specs.filter(m => m.id >= 3000 && m.id < 4000);
```

### Access Model Resources

```javascript
const model = await getObjectModelById('client-id', 3303);

// List all resources
model.resourcedefs.forEach(resource => {
  console.log(`${resource.id}: ${resource.name} (${resource.type})`);
});

// Find specific resource
const sensorValue = model.resourcedefs.find(r => r.id === 5700);
console.log(sensorValue.units); // "Cel"
```

## JSON Response Structure

```typescript
interface ObjectModel {
  id: number;
  name: string;
  instancetype: 'single' | 'multiple';
  mandatory: boolean;
  urn: string;
  version: string;
  lwm2mversion: string;
  description: string;
  description2: string;
  resourcedefs: ResourceModel[];
}

interface ResourceModel {
  id: number;
  name: string;
  operations: string;  // "R", "W", "RW", "E", etc.
  instancetype: 'single' | 'multiple';
  mandatory: boolean;
  type: string;  // "string", "integer", "float", "boolean", etc.
  range: string;
  units: string;
  description: string;
}
```

## Available Models (Examples)

| ID | Name | Instance Type | Resources |
|----|------|---------------|-----------|
| 1 | LWM2M Server | Multiple | 24 |
| 3 | Device | Single | 24 |
| 4 | Connectivity Monitoring | Single | 15 |
| 5 | Firmware Update | Single | 12 |
| 3303 | Temperature | Multiple | 7 |
| 3304 | Humidity | Multiple | 7 |
| 3305 | Power Measurement | Multiple | 10 |
| 3306 | Actuation | Multiple | 6 |

Total: **341 models**

## See Also

- Full Documentation: `webapp/OBJECTSPECS_API.md`
- Java Implementation: `src/main/java/ai/edgez/server/lwm2m/api/ObjectSpecController.java`
- OMA Registry: https://technical.openmobilealliance.org/OMNA/LwM2M/LwM2MRegistry.html
