import React, { useState, useEffect, useMemo } from 'react';
import { io } from 'socket.io-client';

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

export default function LiveTelemetryModal({ onClose }) {
  const [rawData, setRawData] = useState("Connecting to stream...");
  const [telemetry, setTelemetry] = useState(null);

  useEffect(() => {
    // [MODIFICADO] Usar WebSockets en lugar de Polling HTTP
    const socket = io('http://localhost:3001');

    socket.on('connect', () => {
      setRawData("Connected. Waiting for device data...");
    });

    socket.on('telemetry_update', (data) => {
      // Recibimos el string directo desde el servidor
      setRawData(data);
      setTelemetry(parseTelemetry(data));
    });

    socket.on('disconnect', () => {
      setRawData("Disconnected from server.");
    });

    return () => {
      socket.disconnect();
    };
  }, []);

  return (
    <div className="modal-overlay" onClick={onClose}>
      <div className="modal-content" onClick={(e) => e.stopPropagation()} style={{ maxWidth: '600px', flexDirection: 'column', height: 'auto', padding: '0' }}>
        
        <div style={{ padding: '24px', borderBottom: '1px solid var(--border)' }}>
            <h2 style={{ margin: 0 }}>Live Device Telemetry</h2>
            <p style={{ margin: '8px 0 0 0', color: 'var(--text-muted)' }}>Real-time data stream from Watchdog</p>
        </div>

        <div style={{ padding: '32px' }}>
            {telemetry ? (
                <div className="telemetry-viz">
                  {/* AUDIO */}
                  <div style={{ marginBottom: '24px' }}>
                    <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: '0.9rem', marginBottom: '8px', color: 'var(--text-muted)' }}>
                      <span>Audio Level</span>
                      <span>{telemetry.audio} / 4096</span>
                    </div>
                    <div style={{ height: '12px', background: '#334155', borderRadius: '6px', overflow: 'hidden' }}>
                      <div style={{ 
                        width: `${Math.min((telemetry.audio / 4096) * 100, 100)}%`, 
                        height: '100%', 
                        background: telemetry.audio > 2000 ? 'var(--accent-positive)' : 'var(--accent-negative)',
                        transition: 'width 0.3s'
                      }} />
                    </div>
                  </div>

                  {/* AXES */}
                  <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: '20px' }}>
                    {/* HEAD */}
                    <div style={{ textAlign: 'center' }}>
                      <div style={{ fontSize: '0.8rem', color: 'var(--text-muted)', marginBottom: '10px' }}>Heading</div>
                      <div style={{ 
                        width: '80px', height: '80px', 
                        borderRadius: '50%', border: '4px solid var(--border)', 
                        margin: '0 auto', position: 'relative',
                        display: 'flex', alignItems: 'center', justifyContent: 'center',
                        background: 'var(--bg-dark)'
                      }}>
                        <div style={{ 
                          position: 'absolute', width: '4px', height: '50%', 
                          background: 'var(--primary)', top: 0, 
                          transformOrigin: 'bottom center',
                          transform: `rotate(${telemetry.h}deg)`,
                          transition: 'transform 0.3s ease-out'
                        }} />
                        <span style={{ fontSize: '1rem', fontWeight: 'bold' }}>{Math.round(telemetry.h)}°</span>
                      </div>
                    </div>

                    {/* PITCH */}
                    <div style={{ textAlign: 'center' }}>
                      <div style={{ fontSize: '0.8rem', color: 'var(--text-muted)', marginBottom: '10px' }}>Pitch</div>
                      <div style={{ height: '80px', width: '12px', background: '#334155', margin: '0 auto', borderRadius: '6px', position: 'relative' }}>
                         <div style={{
                           position: 'absolute',
                           bottom: '50%', left: 0, right: 0,
                           height: `${(telemetry.p / 90) * 50}%`,
                           background: 'var(--text-main)',
                           borderRadius: '4px',
                           transition: 'height 0.3s ease-out'
                         }} />
                      </div>
                      <span style={{ display:'block', marginTop:'10px', fontSize: '0.9rem' }}>{Math.round(telemetry.p)}°</span>
                    </div>

                    {/* ROLL */}
                    <div style={{ textAlign: 'center' }}>
                      <div style={{ fontSize: '0.8rem', color: 'var(--text-muted)', marginBottom: '10px' }}>Roll</div>
                       <div style={{ 
                         width: '80px', height: '8px', 
                         background: '#334155', margin: '36px auto', 
                         transform: `rotate(${telemetry.r}deg)`,
                         borderRadius: '4px',
                         transition: 'transform 0.3s ease-out'
                       }} />
                       <span style={{ display:'block', marginTop:'10px', fontSize: '0.9rem' }}>{Math.round(telemetry.r)}°</span>
                    </div>
                  </div>
                  
                  <div style={{ marginTop: '30px', textAlign: 'center', fontSize: '1rem', color: 'var(--text-muted)' }}>
                    Total G-Force: <span style={{ color: 'var(--text-main)', fontWeight:'bold' }}>{telemetry.g.toFixed(2)}g</span>
                  </div>

                </div>
            ) : (
                <div style={{ textAlign: 'center', padding: '40px' }}>
                    <div className="pulse" style={{ margin: '0 auto 20px', width: '12px', height: '12px', background: 'var(--primary)' }}></div>
                    <p>{rawData}</p>
                </div>
            )}
        </div>

        <div style={{ padding: '24px', borderTop: '1px solid var(--border)', textAlign: 'right' }}>
            <button className="action-button" style={{ 
                padding: '10px 20px', 
                background: 'var(--bg-hover)', 
                color: 'var(--text-main)', 
                border: 'none', 
                borderRadius: '6px', 
                cursor: 'pointer',
                fontWeight: '600'
            }} onClick={onClose}>
                Close Monitor
            </button>
        </div>

      </div>
    </div>
  );
}