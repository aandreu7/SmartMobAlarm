// Service to fetch events from our local proxy server
// This avoids CORS issues and hides the database keys from the browser

const API_URL = 'http://localhost:3001/api/events';

export const fetchAllEvents = async () => {
  try {
    const response = await fetch(API_URL);
    if (!response.ok) {
      throw new Error(`Server error: ${response.statusText}`);
    }
    const data = await response.json();
    return data;
  } catch (error) {
    console.error("Error fetching events from local API:", error);
    // Return empty array to prevent app crash
    return [];
  }
};