# Generated by Django 3.2.25 on 2024-06-07 16:45

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ('ews', '0008_add_comment_id_for_change'),
    ]

    operations = [
        migrations.AlterField(
            model_name='change',
            name='comment_id',
            field=models.BigIntegerField(default=-1),
        ),
    ]
