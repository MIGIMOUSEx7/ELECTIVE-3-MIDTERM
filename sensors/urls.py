from django.urls import path
from . import views

urlpatterns = [
    path('telemetry/', views.get_telemetry),
    path('alarms/', views.get_alarms),
]