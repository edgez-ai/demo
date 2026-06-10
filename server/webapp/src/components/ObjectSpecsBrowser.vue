<template>
  <v-container>
    <v-card>
      <v-card-title>
        <h2>Object Specifications Browser</h2>
      </v-card-title>
      
      <v-card-text>
        <v-text-field
          v-model="clientEndpoint"
          label="Client Endpoint"
          placeholder="Enter client endpoint (e.g., 12345-7C2C67FFFE53)"
          variant="outlined"
          density="comfortable"
          @keyup.enter="loadSpecs"
        />
        
        <v-btn 
          color="primary" 
          @click="loadSpecs"
          :loading="loading"
          :disabled="!clientEndpoint"
        >
          Load Object Specs
        </v-btn>
        
        <v-alert 
          v-if="error" 
          type="error" 
          class="mt-4"
          closable
          @click:close="error = null"
        >
          {{ error }}
        </v-alert>
        
        <div v-if="objectSpecs.length > 0" class="mt-4">
          <h3>Object Models ({{ objectSpecs.length }})</h3>
          
          <v-text-field
            v-model="searchTerm"
            label="Search"
            prepend-inner-icon="mdi-magnify"
            variant="outlined"
            density="comfortable"
            class="mt-2"
            clearable
          />
          
          <v-list>
            <v-list-item
              v-for="model in filteredSpecs"
              :key="model.id"
              :title="`${model.id} - ${model.name}`"
              :subtitle="model.description"
              @click="selectedModel = model"
            >
              <template v-slot:prepend>
                <v-chip size="small" :color="model.mandatory ? 'primary' : 'default'">
                  {{ model.id }}
                </v-chip>
              </template>
              
              <template v-slot:append>
                <v-chip size="small" variant="outlined">
                  {{ model.instancetype }}
                </v-chip>
              </template>
            </v-list-item>
          </v-list>
        </div>
      </v-card-text>
    </v-card>
    
    <!-- Detail Dialog -->
    <v-dialog v-model="showDetail" max-width="800">
      <v-card v-if="selectedModel">
        <v-card-title>
          {{ selectedModel.name }} (ID: {{ selectedModel.id }})
        </v-card-title>
        
        <v-card-text>
          <div class="mb-4">
            <p><strong>Description:</strong> {{ selectedModel.description }}</p>
            <p><strong>URN:</strong> {{ selectedModel.urn }}</p>
            <p><strong>Version:</strong> {{ selectedModel.version }}</p>
            <p><strong>LwM2M Version:</strong> {{ selectedModel.lwm2mversion }}</p>
            <p><strong>Instance Type:</strong> {{ selectedModel.instancetype }}</p>
            <p><strong>Mandatory:</strong> {{ selectedModel.mandatory ? 'Yes' : 'No' }}</p>
          </div>
          
          <h4>Resources ({{ selectedModel.resourcedefs?.length || 0 }})</h4>
          <v-table density="compact">
            <thead>
              <tr>
                <th>ID</th>
                <th>Name</th>
                <th>Type</th>
                <th>Operations</th>
                <th>Mandatory</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="resource in selectedModel.resourcedefs" :key="resource.id">
                <td>{{ resource.id }}</td>
                <td>{{ resource.name }}</td>
                <td>{{ resource.type }}</td>
                <td>{{ resource.operations }}</td>
                <td>{{ resource.mandatory ? 'Yes' : 'No' }}</td>
              </tr>
            </tbody>
          </v-table>
        </v-card-text>
        
        <v-card-actions>
          <v-spacer></v-spacer>
          <v-btn text @click="showDetail = false">Close</v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>
  </v-container>
</template>

<script setup>
import { ref, computed, watch } from 'vue';
import { fetchObjectSpecs, fetchClientRegistration, filterModelsByRegistration } from '@/js/objectspecs-api.js';

const clientEndpoint = ref('12345-7C2C67FFFE53');
const objectSpecs = ref([]);
const loading = ref(false);
const error = ref(null);
const searchTerm = ref('');
const selectedModel = ref(null);
const showDetail = ref(false);

const filteredSpecs = computed(() => {
  if (!searchTerm.value) return objectSpecs.value;
  
  const term = searchTerm.value.toLowerCase();
  return objectSpecs.value.filter(model => 
    model.name.toLowerCase().includes(term) ||
    model.description.toLowerCase().includes(term) ||
    model.id.toString().includes(term)
  );
});

watch(selectedModel, (newVal) => {
  showDetail.value = !!newVal;
});

async function loadSpecs() {
  if (!clientEndpoint.value) return;
  
  loading.value = true;
  error.value = null;
  objectSpecs.value = [];
  selectedModel.value = null;
  showDetail.value = false;
  
  try {
    const [registration, specs] = await Promise.all([
      fetchClientRegistration(clientEndpoint.value),
      fetchObjectSpecs(clientEndpoint.value)
    ]);

    if (!registration) {
      throw new Error(`no registered client with id '${clientEndpoint.value}'`);
    }

    objectSpecs.value = filterModelsByRegistration(specs, registration);
  } catch (err) {
    error.value = err.message;
  } finally {
    loading.value = false;
  }
}

// Auto-load on mount if endpoint is provided
if (clientEndpoint.value) {
  loadSpecs();
}
</script>

<style scoped>
.v-list-item {
  cursor: pointer;
}

.v-list-item:hover {
  background-color: rgba(0, 0, 0, 0.04);
}
</style>
