import React, { useState } from 'react';

export default function EventCard({ ev }) {
  const [open, setOpen] = useState(false);

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