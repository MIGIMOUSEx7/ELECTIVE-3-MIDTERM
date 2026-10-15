from django.db import models

class TelemetryLog(models.Model):
    timestamp = models.DateTimeField(auto_now_add=True)
    mq2_raw = models.IntegerField()
    mq2_avg = models.FloatField()
    mq2_thresh = models.IntegerField()
    mq7_rise = models.IntegerField()
    mq135_rise = models.IntegerField()

    class Meta:
        ordering = ['-timestamp']

class AlarmEvent(models.Model):
    timestamp = models.DateTimeField(auto_now_add=True)
    source = models.CharField(max_length=50, null=True, blank=True)
    status = models.CharField(max_length=20) # 'ALARM' or 'CLEAR'

    class Meta:
        ordering = ['-timestamp']