from fastapi import FastAPI

app = FastAPI(title="FalSight Main Server")

@app.get("/ping")
async def ping():
    return {"message": "pong", "status": "ok"}