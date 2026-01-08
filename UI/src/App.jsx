// Main app: dashboard logic
import React, { useEffect, useState, useMemo } from 'react';
import EventCard from './components/EventCard';
import LiveTelemetryModal from './components/LiveTelemetryModal';
import { fetchAllEvents } from './services/cosmos';
import './styles.css';

export default function App() {
  const [events, setEvents] = useState([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState(null);
  const [showLive, setShowLive] = useState(false);
  
  // Selection state
  const [selectedEdge, setSelectedEdge] = useState('');
  const [selectedDevice, setSelectedDevice] = useState('ALL');
  const [filterVerdict, setFilterVerdict] = useState('ALL');

  useEffect(() => {
    async function loadData() {
      setLoading(true);
      setError(null);
      try {
        console.log("Fetching events from Cosmos DB...");
        const data = await fetchAllEvents();
        console.log("Events fetched:", data);
        
        if (!data || data.length === 0) {
          console.warn("No data returned from Cosmos DB.");
        }

        setEvents(data || []);
        
        // Default to first edge found
        if (data && data.length > 0) {
          const uniqueEdges = [...new Set(data.map(e => e.edge_id))].filter(Boolean).sort();
          if (uniqueEdges.length > 0) {
            setSelectedEdge(uniqueEdges[0]);
          }
        }
      } catch (err) {
        console.error("Failed to load data:", err);
        setError(err.message || "Unknown error fetching data");
      } finally {
        setLoading(false);
      }
    }

    loadData();
  }, []);

  // 1. Get List of Unique Edges
  const edges = useMemo(() => {
    const allEdges = events.map(e => e.edge_id).filter(Boolean);
    return [...new Set(allEdges)].sort();
  }, [events]);

  // 2. Filter events by Selected Edge
  const edgeEvents = useMemo(() => {
    if (!selectedEdge) return [];
    return events.filter(e => e.edge_id === selectedEdge);
  }, [events, selectedEdge]);

  // 3. Get List of Unique Devices for this Edge
  const devices = useMemo(() => {
    const allDevices = edgeEvents.map(e => e.device_id).filter(Boolean);
    return [...new Set(allDevices)].sort();
  }, [edgeEvents]);

  // Reset device selection when edge changes
  useEffect(() => {
    setSelectedDevice('ALL');
  }, [selectedEdge]);

  // 4. Find the latest REFERENCE image for this Edge (and optionally Device)
  const referenceEvent = useMemo(() => {
    // If a device is selected, try to find a reference specific to that device
    let relevantEvents = edgeEvents;
    if (selectedDevice !== 'ALL') {
      relevantEvents = edgeEvents.filter(e => e.device_id === selectedDevice);
    }
    return relevantEvents.find(e => e.type === 'REFERENCE');
  }, [edgeEvents, selectedDevice]);

  // 5. Calculate stats for this Edge (and filtered Device)
  const stats = useMemo(() => {
    let relevantEvents = edgeEvents;
    if (selectedDevice !== 'ALL') {
      relevantEvents = edgeEvents.filter(e => e.device_id === selectedDevice);
    }

    const positive = relevantEvents.filter(e => e.verdict === 'POSITIVE').length;
    const negative = relevantEvents.filter(e => e.verdict === 'NEGATIVE').length;
    const total = relevantEvents.length;
    return { positive, negative, total };
  }, [edgeEvents, selectedDevice]);

  // 6. Filter for the Grid
  const gridEvents = useMemo(() => {
    // Base: Events for the Edge
    let filtered = edgeEvents.filter(e => e.type !== 'REFERENCE' && e.type !== 'INITIAL_REFERENCE');
    
    // Filter by Device
    if (selectedDevice !== 'ALL') {
      filtered = filtered.filter(e => e.device_id === selectedDevice);
    }

    // Filter by Verdict
    if (filterVerdict !== 'ALL') {
      filtered = filtered.filter(e => e.verdict === filterVerdict);
    }
    return filtered;
  }, [edgeEvents, selectedDevice, filterVerdict]);

  if (loading) return <div className="loading-screen">Loading SmartMobAlarm System...</div>;

  return (
    <div className="app-container">
      {/* Sidebar / Navigation - ALWAYS VISIBLE */}
      <nav className="sidebar">
        <div className="brand">
          <h2>SmartMobAlarm</h2>
          <span className="subtitle">Control Panel</span>
        </div>
        
        <div className="nav-section">
          <label className="nav-label">Edge Nodes</label>
          <select 
            value={selectedEdge} 
            onChange={(e) => setSelectedEdge(e.target.value)}
            className="edge-selector"
          >
            {edges.length > 0 ? (
              edges.map(edge => <option key={edge} value={edge}>{edge}</option>)
            ) : (
              <option value="">No Edges Active</option>
            )}
          </select>
        </div>

        {edges.length > 0 && (
          <div className="nav-section">
            <label className="nav-label">Device Channels</label>
            <select 
              value={selectedDevice} 
              onChange={(e) => setSelectedDevice(e.target.value)}
              className="edge-selector"
            >
              <option value="ALL">All Devices</option>
              {devices.map(dev => (
                <option key={dev} value={dev}>{dev}</option>
              ))}
            </select>
          </div>
        )}

        <div className="nav-section stats-summary">
          <button 
            onClick={() => setShowLive(true)}
            style={{
              width: '100%',
              padding: '12px',
              backgroundColor: 'rgba(239, 68, 68, 0.15)',
              border: '1px solid var(--accent-positive)',
              color: 'var(--accent-positive)',
              borderRadius: '8px',
              cursor: 'pointer',
              fontWeight: 'bold',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              gap: '8px',
              marginBottom: '20px'
            }}
          >
            <span className="pulse" style={{background: 'var(--accent-positive)', boxShadow: 'none', width: '8px', height: '8px'}}></span>
            Live Device Monitor
          </button>

          <label className="nav-label">Real-time Metrics</label>
          <div className="stat-item">
            <span>Positive Alerts</span>
            <span className="stat-value" style={{color: 'var(--accent-positive)'}}>{stats.positive}</span>
          </div>
          <div className="stat-item">
            <span>False Positives</span>
            <span className="stat-value" style={{color: 'var(--accent-negative)'}}>{stats.negative}</span>
          </div>
        </div>
      </nav>

      {/* Main Content */}
      <main className="main-content">
        <header className="top-bar">
          <div className="header-info">
            <h1>{selectedEdge || "System Standby"}</h1>
            <p className="status-badge"><span className="pulse"></span> System Online</p>
          </div>
          <div className="user-profile">
            <div className="avatar">A</div>
          </div>
        </header>

        {/* Hero Section */}
        <section className="hero-grid">
          <div className="reference-card">
            <div className="card-header-flex">
              <h3>Live Reference</h3>
              {referenceEvent && <span className="timestamp-badge">Updated: {new Date(referenceEvent.timestamp).toLocaleTimeString()}</span>}
            </div>
            
            <div className="reference-image-container">
              {referenceEvent && referenceEvent.image_url && referenceEvent.image_url !== 'NO_IMAGE' ? (
                <>
                  <img src={referenceEvent.image_url} alt="Reference" />
                  <div className="scan-line"></div>
                </>
              ) : (
                <div className="placeholder-viz">
                  <div className="crosshair"></div>
                  <span>Waiting for Reference Frame...</span>
                </div>
              )}
            </div>
          </div>
          
          <div className="stats-cards">
            <div className={`stat-box positive ${filterVerdict === 'POSITIVE' ? 'active-filter' : ''}`} onClick={() => setFilterVerdict('POSITIVE')}>
               <h4>Critical Detection</h4>
               <div className="big-number">{stats.positive}</div>
               <div className="stat-trend">Requires Attention</div>
            </div>
            <div className={`stat-box negative ${filterVerdict === 'NEGATIVE' ? 'active-filter' : ''}`} onClick={() => setFilterVerdict('NEGATIVE')}>
               <h4>Verified Safe</h4>
               <div className="big-number">{stats.negative}</div>
               <div className="stat-trend">Identity Confirmed</div>
            </div>
          </div>
        </section>

        {/* Events Feed */}
        <section className="events-section">
          <div className="events-header">
            <h3>Detection History</h3>
            <div className="tabs">
              <button className={filterVerdict === 'ALL' ? 'active' : ''} onClick={() => setFilterVerdict('ALL')}>All</button>
              <button className={filterVerdict === 'POSITIVE' ? 'active' : ''} onClick={() => setFilterVerdict('POSITIVE')}>Alerts</button>
              <button className={filterVerdict === 'NEGATIVE' ? 'active' : ''} onClick={() => setFilterVerdict('NEGATIVE')}>Safe</button>
            </div>
          </div>

          <div className="events-grid">
            {gridEvents.length > 0 ? (
              gridEvents.map(ev => <EventCard key={ev.id} ev={ev} />)
            ) : (
              <div className="empty-state-v2">
                <div className="radar-icon"></div>
                <h4>No recent incidents detected</h4>
                <p>System is monitoring for movement and biometric matches...</p>
              </div>
            )}
          </div>
        </section>
      </main>

      {showLive && <LiveTelemetryModal onClose={() => setShowLive(false)} />}
    </div>
  );
}
