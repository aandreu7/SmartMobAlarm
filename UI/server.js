require('dotenv').config();
const express = require('express');
const cors = require('cors');
const { CosmosClient } = require('@azure/cosmos');

const app = express();
app.use(cors()); // Permite peticiones desde el Front-end (localhost:5173)
app.use(express.json());

// Cargar variables de entorno
const ENDPOINT = process.env.VITE_COSMOS_ENDPOINT;
const KEY = process.env.VITE_COSMOS_KEY;
const DB_NAME = process.env.VITE_COSMOS_DB_NAME;
const CONTAINER_NAME = process.env.VITE_COSMOS_CONTAINER_NAME;

// Inicializar cliente Cosmos DB
let container = null;
if (ENDPOINT && KEY) {
    try {
        const client = new CosmosClient({ endpoint: ENDPOINT, key: KEY });
        container = client.database(DB_NAME).container(CONTAINER_NAME);
        console.log("✅ Conexión a Cosmos DB inicializada en el servidor.");
    } catch (e) {
        console.error("❌ Error conectando a Cosmos DB:", e.message);
    }
} else {
    console.error("⚠️ Faltan credenciales VITE_COSMOS_* en el archivo .env");
}

// Endpoint API
app.get('/api/events', async (req, res) => {
    if (!container) {
        return res.status(500).json({ error: "Base de datos no configurada en el servidor." });
    }
    try {
        console.log("📥 Recibiendo petición de eventos...");
        const querySpec = {
            query: "SELECT * FROM c ORDER BY c.timestamp DESC"
        };
        const { resources: items } = await container.items.query(querySpec).fetchAll();
        console.log(`📤 Enviando ${items.length} eventos al cliente.`);
        res.json(items);
    } catch (error) {
        console.error("❌ Error en consulta Cosmos:", error.message);
        res.status(500).json({ error: error.message });
    }
});

const PORT = 3001;
app.listen(PORT, () => {
    console.log(`\n🚀 Servidor API corriendo en http://localhost:${PORT}`);
    console.log(`   (El Frontend debe solicitar datos a esta URL)\n`);
});
