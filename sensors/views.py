from rest_framework.decorators import api_view
from rest_framework.response import Response
from .models import TelemetryLog, AlarmEvent

@api_view(['GET'])
def get_telemetry(request):
    # Fetch the 50 most recent readings
    logs = TelemetryLog.objects.all()[:50]
    data = [{
        "time": log.timestamp, 
        "mq2_avg": log.mq2_avg, 
        "mq7_rise": log.mq7_rise, 
        "mq135_rise": log.mq135_rise
    } for log in logs]
    return Response(data)

@api_view(['GET'])
def get_alarms(request):
    # Fetch the 20 most recent alarm events
    alarms = AlarmEvent.objects.all()[:20]
    data = [{"time": a.timestamp, "source": a.source, "status": a.status} for a in alarms]
    return Response(data)