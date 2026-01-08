import React, { useState, useMemo } from 'react';

const parseTelemetry = (str) => {
  if (!str || typeof str !== 'string' || !str.includes('Au:')) return null;
  // Format: "Au:1500 | G:1.02 | H:120 P:-5 R:10"
  try {
    const data = { audio: 0, g: 0, h: 0, p: 0, r: 0 };
    const parts = str.split('|');
    
    parts.forEach(part => {
      const p = part.trim();
      if (p.startsWith('Au:')) data.audio = parseInt(p.split(':')[1], 10);
      else if (p.startsWith('G:')) data.g = parseFloat(p.split(':')[1]);
      else if (p.startsWith('H:')) {
        // "H:120 P:-5 R:10"
        const sub = p.split(/\s+/);
        sub.forEach(s => {
          if (s.startsWith('H:')) data.h = parseFloat(s.split(':')[1]);
          if (s.startsWith('P:')) data.p = parseFloat(s.split(':')[1]);
          if (s.startsWith('R:')) data.r = parseFloat(s.split(':')[1]);
        });
      }
    });
    return data;
  } catch (e) {
    console.warn("Telemetry parse error:", e);
    return null;
  }
};

export default function EventCard({ ev }) {
  const [open, setOpen] = useState(false);
  const [showExtended, setShowExtended] = useState(false);

  const telemetry = useMemo(() => parseTelemetry(ev.telemetry_snapshot), [ev.telemetry_snapshot]);

  // Helper to format date nicely
  const formatDate = (isoString) => {
    const d = new Date(isoString);
    return d.toLocaleString(undefined, {
      month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit'
    });
  };

  const badgeClass = ev.verdict === 'POSITIVE' ? 'badge-positive' : 
                     ev.verdict === 'NEGATIVE' ? 'badge-negative' : 'badge-info';

  const hasImage = ev.image_url && ev.image_url !== 'NO_IMAGE';
  const bgStyle = hasImage ? { backgroundImage: `url(${ev.image_url})` } : { backgroundColor: '#334155' };

  return (
    <>
      <article className="event-card" onClick={() => setOpen(true)}>
        <div 
          className="event-media" 
          style={bgStyle}
        >
          {!hasImage && <div style={{display:'flex', height:'100%', alignItems:'center', justifyContent:'center', color:'#94a3b8'}}>No Image</div>}
          <div className={`event-badge ${badgeClass}`}>
            {ev.verdict}
          </div>
        </div>
        
        <div className="event-body">
          <div className="event-meta">
            <span className="event-type">{ev.type}</span>
            <span className="event-time">{formatDate(ev.timestamp)}</span>
          </div>
          
          <div className="event-reasons">
            {ev.reasons && ev.reasons.join(', ')}
          </div>
        </div>
      </article>

      {open && (
        <div className="modal-overlay" onClick={() => setOpen(false)}>
          <div className="modal-content" onClick={(e) => e.stopPropagation()}>
            <div className="modal-image">
              {hasImage ? (
                <img src={ev.image_url} alt={ev.type} />
              ) : (
                <div style={{color:'#94a3b8'}}>No Image Available</div>
              )}
            </div>
            
            <div className="modal-details">
              <div className="modal-header">
                <h2>{ev.verdict} Alert</h2>
                <div className="modal-info-value" style={{color: 'var(--primary)'}}>
                  {ev.type}
                </div>
              </div>

              <div className="modal-info-row">
                <span className="modal-info-label">Timestamp</span>
                <span className="modal-info-value">{new Date(ev.timestamp).toLocaleString()}</span>
              </div>
              
              <div className="modal-info-row">
                <span className="modal-info-label">Device</span>
                <span className="modal-info-value">{ev.device_id}</span>
              </div>
              
              <div className="modal-info-row">
                <span className="modal-info-label">Edge Node</span>
                <span className="modal-info-value">{ev.edge_id}</span>
              </div>

              {telemetry && (
                <div style={{ marginBottom: '20px', marginTop: '10px' }}>
                  <button 
                    className="action-button"
                    onClick={() => setShowExtended(!showExtended)}
                    style={{
                      width: '100%',
                      padding: '10px',
                      background: 'rgba(255,255,255,0.05)',
                      border: '1px solid var(--border)',
                      color: 'var(--primary)',
                      cursor: 'pointer',
                      borderRadius: '8px'
                    }}
                  >
                    {showExtended ? "Hide Device Telemetry" : "Show Extended Device Information"}
                  </button>

                  {showExtended && (
                    <div className="telemetry-viz" style={{ marginTop: '15px', padding: '15px', background: 'rgba(0,0,0,0.2)', borderRadius: '8px' }}>
                      
                      {/* AUDIO */}
                      <div style={{ marginBottom: '15px' }}>
                        <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: '0.8rem', marginBottom: '5px', color: 'var(--text-muted)' }}>
                          <span>Audio Level</span>
                          <span>{telemetry.audio} / 4096</span>
                        </div>
                        <div style={{ height: '8px', background: '#334155', borderRadius: '4px', overflow: 'hidden' }}>
                          <div style={{ 
                            width: `${Math.min((telemetry.audio / 4096) * 100, 100)}%`, 
                            height: '100%', 
                            background: telemetry.audio > 2000 ? 'var(--accent-positive)' : 'var(--accent-negative)',
                            transition: 'width 0.3s'
                          }} />
                        </div>
                      </div>

                      {/* AXES */}
                      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: '10px' }}>
                        {/* HEAD */}
                        <div style={{ textAlign: 'center' }}>
                          <div style={{ fontSize: '0.75rem', color: 'var(--text-muted)', marginBottom: '5px' }}>Heading</div>
                          <div style={{ 
                            width: '40px', height: '40px', 
                            borderRadius: '50%', border: '2px solid var(--border)', 
                            margin: '0 auto', position: 'relative',
                            display: 'flex', alignItems: 'center', justifyContent: 'center'
                          }}>
                            <div style={{ 
                              position: 'absolute', width: '2px', height: '50%', 
                              background: 'var(--primary)', top: 0, 
                              transformOrigin: 'bottom center',
                              transform: `rotate(${telemetry.h}deg)`
                            }} />
                            <span style={{ fontSize: '0.7rem' }}>{Math.round(telemetry.h)}°</span>
                          </div>
                        </div>

                        {/* PITCH */}
                        <div style={{ textAlign: 'center' }}>
                          <div style={{ fontSize: '0.75rem', color: 'var(--text-muted)', marginBottom: '5px' }}>Pitch</div>
                          <div style={{ height: '40px', width: '8px', background: '#334155', margin: '0 auto', borderRadius: '4px', position: 'relative' }}>
                             <div style={{
                               position: 'absolute',
                               bottom: '50%', left: 0, right: 0,
                               height: `${(telemetry.p / 90) * 50}%`,
                               background: 'var(--text-main)',
                               borderRadius: '4px'
                             }} />
                          </div>
                          <span style={{ fontSize: '0.7rem' }}>{Math.round(telemetry.p)}°</span>
                        </div>

                        {/* ROLL */}
                        <div style={{ textAlign: 'center' }}>
                          <div style={{ fontSize: '0.75rem', color: 'var(--text-muted)', marginBottom: '5px' }}>Roll</div>
                           <div style={{ 
                             width: '40px', height: '4px', 
                             background: '#334155', margin: '18px auto', 
                             transform: `rotate(${telemetry.r}deg)`
                           }} />
                           <span style={{ fontSize: '0.7rem' }}>{Math.round(telemetry.r)}°</span>
                        </div>
                      </div>
                      
                      <div style={{ marginTop: '15px', textAlign: 'center', fontSize: '0.8rem', color: 'var(--text-muted)' }}>
                        G-Force: <span style={{ color: 'var(--text-main)' }}>{telemetry.g.toFixed(2)}g</span>
                      </div>

                    </div>
                  )}
                </div>
              )}

              <div className="reasons-list">
                <h4>Detected Reasons</h4>
                <ul>
                  {ev.reasons && ev.reasons.map((reason, idx) => (
                    <li key={idx}>{reason}</li>
                  ))}
                </ul>
              </div>

              <button className="close-button" onClick={() => setOpen(false)}>
                Close Detail View
              </button>
            </div>
          </div>
        </div>
      )}
    </>
  );
}