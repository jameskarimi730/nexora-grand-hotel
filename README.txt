NEXORA GRAND HOTEL — COMPLETE WEBSITE PROJECT
================================================

WHAT IS INCLUDED
- Multi-page hotel website: Home, Rooms, Booking, My Reservation, About, Contact.
- Responsive design for desktop and mobile.
- C language backend (backend/main.c).
- SQLite database for saving reservations.
- Availability check against sample room inventory.
- Booking reference generation (NX000001, etc.).
- Reservation lookup using booking reference.
- Dockerfile for deploying the C backend.

IMPORTANT HOSTING ARCHITECTURE
Netlify hosts the static website files. It does not run this long-running C server.
Deploy the backend folder as a Docker web service on a host that supports Docker and persistent disks.
The database must be stored on a persistent disk, otherwise reservations can be lost when a host restarts/redeploys.

SAMPLE ROOM SETTINGS (CHANGE BEFORE REAL HOTEL USE)
Standard Room: KSh 3,500/night, capacity 10
Deluxe Room: KSh 5,500/night, capacity 6
Executive Suite: KSh 8,500/night, capacity 3

These are sample project values, not verified hotel rates or actual inventory.
The current capacity logic limits overlapping reservations by room category; it is a starter inventory model.
No payment gateway is included. The site does not collect money.
No guest email/SMS confirmation is sent yet.

A. DEPLOY THE C BACKEND
1. Create a GitHub account if you do not already have one.
2. Upload the complete project folder to a new GitHub repository.
3. On a Docker-capable hosting provider (for example Render), create a new Web Service from that repository.
4. Set the Root Directory to: backend
5. Choose Docker as the runtime.
6. Add a persistent disk and mount it at: /var/data
7. Set environment variable DB_PATH to: /var/data/nexora.db
8. The service uses the PORT environment variable supplied by the host.
9. Deploy. Copy the public HTTPS service URL.
10. Open https://YOUR-BACKEND-URL/health. It should return JSON with success:true.

IMPORTANT: Persistent disks may require a paid hosting plan. Do not accept real bookings on an ephemeral filesystem.
The backend currently uses a simple single-process HTTP server and SQLite. For a high-traffic commercial hotel,
use a hardened production web server, TLS termination, rate limiting, backups, monitoring and a reviewed database design.

B. CONNECT THE WEBSITE TO THE C BACKEND
1. Open config.js in the project root.
2. Replace https://YOUR-C-BACKEND-URL with your backend HTTPS URL, without a trailing slash.
   Example: const API_BASE = "https://nexora-hotel-api.onrender.com";
3. Save config.js.
4. Commit/push the change to GitHub.

C. DEPLOY THE WEBSITE TO NETLIFY
1. Sign in to Netlify.
2. Choose Add new site / Import an existing project.
3. Connect the GitHub repository containing this project.
4. Build command: leave blank.
5. Publish directory: leave as . (or use the repository root).
6. Deploy the site.
7. Open the Netlify URL and test all pages.

D. TEST A REAL BOOKING
1. Open the Netlify website and choose Book a stay.
2. Enter a test guest name, email, phone, room and future dates.
3. Submit. The system should return a booking reference such as NX000001.
4. Open My Reservation in a new tab and enter that reference.
5. Confirm that the saved reservation details appear.
6. Try another overlapping booking for the same room type. The backend should enforce the configured sample capacity.

FILES
index.html         Home
rooms.html         Room descriptions and sample rates
booking.html       Booking form
reservations.html  Reservation lookup
about.html         About page
contact.html       Contact page
style.css          Website styling
config.js          C backend URL setting
app.js             Browser-side booking and lookup logic
backend/main.c     C HTTP API and SQLite booking logic
backend/Dockerfile Backend build instructions
netlify.toml       Netlify static publishing settings

BEFORE GOING LIVE WITH CUSTOMERS
- Replace sample room prices and capacities with the hotel's real inventory.
- Replace placeholder contact details and confirm legal business details.
- Add a proper privacy notice and terms of booking.
- Add cancellation/modification rules.
- Configure automated database backups.
- Add payment integration only after obtaining merchant credentials and testing it.
- Have the C backend reviewed and security-hardened before commercial production.

C BACKEND LOCAL BUILD (Linux)
  gcc -O2 -Wall -Wextra -o nexora-api backend/main.c -lsqlite3
  mkdir -p data
  DB_PATH=./data/nexora.db PORT=8080 ./nexora-api

Then test:
  curl http://localhost:8080/health
